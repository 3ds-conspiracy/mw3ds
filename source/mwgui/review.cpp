// Character creation: the review screen
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

// Skills rise with use (SKDT use values): each point needs skillNeed(skill) progress; the rest of the
// rules (level progress, attribute multipliers) are raiseSkill's
void Session::useSkill(int skill, int use, float amount)
{
	PlayerStats& s = w.stats;
	if (skill < 0 || skill >= 27 || skill >= (int)w.game.skills.size() || s.skills[skill] >= 100 || !w.playerClass())
		return;
	s.skillProgress[skill] += w.game.skills[skill].use[use & 3] * amount;
	if (s.skillProgress[skill] < skillNeed(skill))
		return;
	s.skillProgress[skill] = 0.0f;
	raiseSkill(skill);
}

void Session::drawReview()
{
	header("Review your information");
	const PlayerStats& s = w.stats;
	const GameData& g = w.game;
	// health, magicka, fatigue and the attributes on the top screen
	topModal.kind = TopModal::VITALS;
	topModal.title = "";
	topModal.vitals[0][0] = topModal.vitals[0][1] = (int)s.healthMax;
	topModal.vitals[1][0] = topModal.vitals[1][1] = (int)s.magickaMax;
	topModal.vitals[2][0] = topModal.vitals[2][1] = (int)s.fatigueMax;
	topModal.attributes = true;

	std::string race, cls, sign;
	for (auto& r : g.races)
		if (lower(r.id) == lower(s.race))
			race = r.name;
	const ClassDef* c = w.playerClass();
	if (c)
		cls = c->name;
	if (const BirthDef* b = w.birthsign())
		sign = b->name;
	// who: name and race, class and sign
	const float sc = 0.46f, lh = uiLineHeight(sc) + 2;
	auto fact = [&](float x, float y, const char* key, const char* fallback, const std::string& value) {
		std::string label = g.gmstLabel(key, fallback) + ":";
		uiText(x, y, sc, col::textDim, label);
		float vx = x + uiTextWidth(label, sc) + 5;
		std::string v = value;
		while (v.size() > 3 && vx + uiTextWidth(v, sc) > x + 152)
			v = v.substr(0, v.size() - 4) + "...";
		uiText(vx, y, sc, col::header, v);
	};
	uiPanel(4, 24, 312, 2 * lh + 6);
	fact(10, 27, "sname", "Name", s.name);
	fact(164, 27, "srace", "Race", race);
	fact(10, 27 + lh, "sclass", "Class", cls);
	fact(164, 27 + lh, "sbirthsign", "Sign", sign);
	// the skills under their class headings: a list that scrolls
	std::vector<std::string> rows, right;
	auto inList = [](const int* arr, int k) { for (int i = 0; i < 5; i++) if (arr[i] == k) return true; return false; };
	auto section = [&](const std::string& title, auto pred) {
		rows.push_back(title);
		right.push_back("");
		for (int k = 0; k < 27; k++)
			if (pred(k))
			{
				rows.push_back("   " + skillName(w, k));
				right.push_back(std::to_string(s.skills[k]));
			}
	};
	if (c)
	{
		section(g.gmstLabel("sskillclassmajor", "Major Skills"), [&](int k) { return inList(c->major, k); });
		section(g.gmstLabel("sskillclassminor", "Minor Skills"), [&](int k) { return inList(c->minor, k); });
		section(g.gmstLabel("sskillclassmisc", "Misc Skills"), [&](int k) { return !inList(c->major, k) && !inList(c->minor, k); });
	}
	float sy = 24 + 2 * lh + 10;
	uiPanel(4, sy, 312, 204 - sy);
	uiList(list, 5, sy + 1, 310, 202 - sy, rows, 0.44f, true, &right);
	if (buttonRow({ g.gmst("sok", "OK") }, focus) == 0)
	{
		logf("chargen: review accepted");
		givePlayerStartSpells();
		closeScreen();
	}
}
