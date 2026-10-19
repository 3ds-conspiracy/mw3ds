// Character creation: the birthsign menu
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwgui/statshelpers.hpp"

void Session::drawBirth()
{
	header("Choose your birthsign");
	std::vector<std::string> rows;
	for (auto& b : w.game.birthsigns)
		rows.push_back(b.name);
	int pick = uiList(list, 4, 24, 116, 212, rows);
	const BirthDef& b = w.game.birthsigns[list.selected < (int)rows.size() ? list.selected : 0];
	// the constellation, in a box on the top screen
	auto art = w.game.art.birthsign.find(lower(b.id));
	if (art != w.game.art.birthsign.end() && art->second.h > 0)
	{
		topModal.kind = TopModal::ART;
		topModal.art = art->second;
		topModal.title = b.name;
	}
	// What the sign is, then what it gives (OpenMW's birthsign menu): abilities, powers, spells, each with its
	// effects and their icons
	const float scale = 0.4f;
	std::vector<std::string> lines;
	std::vector<int> icons;
	auto add = [&](const std::string& s, int icon = -1) { lines.push_back(s); icons.push_back(icon); };
	for (auto& l : uiWrap(b.desc, 176, scale))
		add(l);
	static const struct { int type; const char* key; const char* text; } kGroups[] = {
		{ 1, "sbirthsignmenu1", "Abilities:" }, { 5, "sbirthsignmenu2", "Powers:" }, { 0, "sbirthsignmenu3", "Spells:" } };
	for (auto& g : kGroups)
	{
		bool head = false;
		for (auto& id : b.spells)
		{
			auto it = w.game.spells.find(lower(id));
			if (it == w.game.spells.end() || it->second.type != g.type)
				continue;
			if (!head)
			{
				add("");
				add(w.game.gmstLabel(g.key, g.text));
				head = true;
			}
			add(it->second.name);
			for (auto& e : it->second.effects)
			{
				auto me = w.game.magicEffects.find(e.effect);
				std::vector<std::string> wrapped = uiWrap(effectLine(e, false), 150, scale);
				for (size_t k = 0; k < wrapped.size(); k++)
					add(wrapped[k], k == 0 && me != w.game.magicEffects.end() ? me->second.iconIx : -1);
			}
		}
	}
	static int shown = -1;
	if (shown != list.selected)
	{
		shown = list.selected;
		list2.scroll = 0.0f;
	}
	uiPanel(124, 24, 192, 168);
	birthIcons = 0;
	for (int ic : icons)
		birthIcons += ic >= 0;
	uiList(list2, 125, 25, 190, 166, lines, scale, false, nullptr, &icons);
	if (uiButton(124, 196, 192, 36, w.game.gmst("sok", "OK"), true) || (pick >= 0 && (uiIn().down & KEY_A)))
	{
		w.stats.birthsign = b.id;
		w.recomputeStats();
		logf("chargen: birthsign %s", b.id.c_str());
		closeScreen();
	}
}
