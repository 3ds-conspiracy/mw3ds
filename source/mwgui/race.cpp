// Character creation: the race menu and face preview
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

void Session::drawRace()
{
	header("Choose your race");
	std::vector<const RaceDef*> races;
	std::vector<std::string> rows;
	for (auto& r : w.game.races)
		if (r.playable)
		{
			races.push_back(&r);
			rows.push_back(r.name);
		}
	int pick = uiList(list, 4, 24, 116, 212, rows);
	const RaceDef* r = races[list.selected < (int)races.size() ? list.selected : 0];
	// the face in the preview (top screen) follows the choice at once
	if (lower(w.stats.race) != lower(r->id))
	{
		w.stats.race = r->id;
		w.stats.head.clear();
		w.stats.hair.clear();
	}

	std::string text = r->desc + "\n\nSkill bonuses:\n";
	for (auto& b : r->skillBonus)
		text += "  " + skillName(w, b.first) + " +" + std::to_string(b.second) + "\n";
	text += "\nAttributes (" + std::string(w.stats.female ? "female" : "male") + "):\n";
	for (int i = 0; i < 8; i++)
		text += "  " + attrName(w, i) + " " + std::to_string(r->attributes[i][w.stats.female ? 1 : 0]) + "\n";
	uiPanel(124, 24, 192, 96);
	uiTextBox(scroll, 126, 26, 188, 92, text, 0.45f, false);

	const UiInput& in = uiIn();
	// The C-stick turns the face on the top screen (its camera goes round the head), 150 degrees a second at full push
	previewSpin += in.stickX * 2.6f * in.dt;
	uiText(128, 226, 0.33f, col::textDim, "C-stick: turn the face");     // (under OK, above the screen's edge)
	// Head and hair: < > through the race's own (the face shows on the top screen)
	for (int hair = 0; hair < 2; hair++)
	{
		const auto* list2 = playerHeadChoices(w, hair != 0);
		float y = 124 + hair * 20;
		int n = list2 ? (int)list2->size() : 0, cur = 0;
		std::string& chosen = hair ? w.stats.hair : w.stats.head;
		for (int k = 0; k < n; k++)
			if ((*list2)[k].first == chosen)
				cur = k;
		uiText(128, y + 3, 0.42f, col::text, std::string(hair ? w.game.gmst("shair", "Hair") : w.game.gmst("sface", "Face"))
			+ "  " + std::to_string(n ? cur + 1 : 0) + " / " + std::to_string(n));
		int step = 0;
		if (uiButton(252, y, 30, 18, "<")) step = -1;
		if (uiButton(286, y, 30, 18, ">")) step = 1;
		if (step && n)
		{
			cur = (cur + step + n) % n;
			chosen = (*list2)[cur].first;
			playSound(-1, "Menu Click");
		}
	}
	if (uiButton(124, 164, 94, 24, "Male", !w.stats.female) || (in.down & KEY_L)) w.stats.female = false;
	if (uiButton(222, 164, 94, 24, "Female", w.stats.female) || (in.down & KEY_R)) w.stats.female = true;
	if (uiButton(124, 196, 192, 28, w.game.gmst("sok", "OK"), true) || (pick >= 0 && (in.down & KEY_A)))
	{
		w.stats.race = r->id;
		w.recomputeStats();
		logf("chargen: race %s %s", r->id.c_str(), w.stats.female ? "female" : "male");
		closeScreen();
	}
}
