// The level-up screen: three attributes and their multipliers
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// Morrowind's level-up window: the new level's words (Morrowind.ini), then three attributes to raise,
// each by its multiplier
void Session::drawLevelUp()
{
	PlayerStats& s = w.stats;
	const UiInput& in = uiIn();
	int next = s.level + 1;
	uiCaption(w.game.gmst("slevelup", "Level Up"), w.game.gmst("slevel", "Level") + " " + std::to_string(next));
	auto words = w.game.levelUpText.find("level" + std::to_string(next));
	if (words == w.game.levelUpText.end())
		words = w.game.levelUpText.find("default");
	std::string text = w.game.gmst("slevelupmenu1", "You have ascended to Level ") + std::to_string(next) + ".";
	if (words != w.game.levelUpText.end())
		text += "\n" + words->second;
	uiPanel(4, 24, 312, 68);
	// Morrowind's picture for the class, when it has one, beside the words
	auto art = w.game.art.levelup.find(lower(s.cls));
	bool pic = art != w.game.art.levelup.end() && art->second.h > 0;
	if (pic)
	{
		float ph = 64.0f, pw = art->second.w * ph / art->second.h;
		uiArt(art->second.file, art->second.w, art->second.h, 314 - pw, 26, pw, ph);
		uiTextBox(scroll, 6, 26, 308 - pw - 4, 64, text, 0.4f);
	}
	else
		uiTextBox(scroll, 6, 26, 308, 64, text, 0.4f);

	int open = 0;
	for (int i = 0; i < 8; i++)
		open += s.attributes[i] < 100;
	size_t need = open < 3 ? open : 3;

	// D-pad: two columns of four, the OK button below them
	if (in.down & KEY_DOWN)
		levelCursor = levelCursor == 8 || levelCursor % 4 == 3 ? 8 : levelCursor + 1;
	if (in.down & KEY_UP)
		levelCursor = levelCursor == 8 ? 3 : levelCursor % 4 == 0 ? levelCursor : levelCursor - 1;
	if ((in.down & (KEY_LEFT | KEY_RIGHT)) && levelCursor < 8)
		levelCursor = (levelCursor + 4) % 8;
	int toggle = -1;
	if ((in.down & KEY_A) && levelCursor < 8)
		toggle = levelCursor;
	for (int i = 0; i < 8; i++)
	{
		float x = 6 + (i / 4) * 156, y = 98 + (i % 4) * 26;
		bool can = s.attributes[i] < 100;
		bool picked = std::find(levelPicks.begin(), levelPicks.end(), i) != levelPicks.end();
		uiRect(x, y, 152, 24, picked ? col::select : col::panelLight);
		uiFrame(x, y, 152, 24, "menu_button_frame", i == levelCursor ? col::header : col::border);
		if (i == levelCursor)
			uiFrame(x + 1, y + 1, 150, 22, "menu_thin_border", col::header);
		u32 c = !can ? col::textDim : picked ? col::textPressed : col::text;
		uiText(x + 6, y + 4, 0.45f, c, attrName(w, i));
		uiTextRight(x + 114, y + 4, 0.45f, c, std::to_string(s.attributes[i]));
		int m = attrMultiplier(i);
		if (m > 1 && can)
			uiTextRight(x + 146, y + 4, 0.45f, picked ? col::textPressed : col::header, "x" + std::to_string(m));
		if (can && uiHit(x, y, 152, 24))
		{
			toggle = i;
			levelCursor = i;
		}
	}
	if (toggle >= 0 && s.attributes[toggle] < 100)
	{
		// Picking a fourth replaces the last one picked (OpenMW's rule)
		auto it = std::find(levelPicks.begin(), levelPicks.end(), toggle);
		if (it != levelPicks.end())
			levelPicks.erase(it);
		else if (levelPicks.size() < need)
			levelPicks.push_back(toggle);
		else if (!levelPicks.empty())
			levelPicks.back() = toggle;
		playSound(-1, "Menu Click");
	}
	if (uiButton(110, 206, 100, 30, w.game.gmst("sok", "OK"), levelCursor == 8, levelPicks.size() == need))
	{
		applyLevelUp();
		closeScreen();
	}
}
