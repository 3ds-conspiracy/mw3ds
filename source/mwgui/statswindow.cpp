// The character: journal, stats, character creation, level progress, death and the end
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

std::string statsText(Session& ss, bool progress)
{
	World& w = ss.w;
	const PlayerStats& s = w.stats;
	std::string race, cls, sign;
	for (auto& r : w.game.races)
		if (lower(r.id) == lower(s.race)) race = r.name;
	const ClassDef* c = w.playerClass();
	if (c) cls = c->name;
	if (const BirthDef* b = w.birthsign()) sign = b->name;
	char buf[128];
	const GameData& g = w.game;
	std::string t = g.gmstLabel("sname", "Name") + ": " + s.name + "\n" + g.gmstLabel("srace", "Race") + ": " + race +
		(s.female ? " (female)" : " (male)") + "\n" + g.gmstLabel("sclass", "Class") + ": " + cls + "\n" +
		g.gmstLabel("sbirthsign", "Sign") + ": " + sign +
		(progress ? "" : "\n" + g.gmstLabel("slevel", "Level") + ": " + std::to_string(s.level)) + "\n\n";
	snprintf(buf, sizeof(buf), "%s %d   %s %d   %s %d\n\n", g.gmstLabel("shealth", "Health").c_str(), (int)s.healthMax,
		g.gmstLabel("smagic", "Magicka").c_str(), (int)s.magickaMax, g.gmstLabel("sfatigue", "Fatigue").c_str(), (int)s.fatigueMax);
	t += buf;
	for (int i = 0; i < 8; i++)
		t += attrName(w, i) + ": " + std::to_string(s.attributes[i]) + (i % 2 ? "\n" : "     ");
	auto section = [&](const std::string& title, auto pred) {
		t += std::string("\n") + title + "\n";
		for (int k = 0; k < 27; k++)
			if (pred(k))
			{
				t += "  " + skillName(w, k) + ": " + std::to_string(s.skills[k]);
				// Progress towards the next point
				if (progress && s.skills[k] < 100)
					t += "  (" + std::to_string((int)(100.0f * s.skillProgress[k] / ss.skillNeed(k))) + "%)";
				t += "\n";
			}
	};
	auto inList = [](const int* arr, int k) { for (int i = 0; i < 5; i++) if (arr[i] == k) return true; return false; };
	if (c)
	{
		section(w.game.gmstLabel("sskillclassmajor", "Major Skills"), [&](int k) { return inList(c->major, k); });
		section(w.game.gmstLabel("sskillclassminor", "Minor Skills"), [&](int k) { return inList(c->minor, k); });
		section(w.game.gmstLabel("sskillclassmisc", "Misc Skills"), [&](int k) { return !inList(c->major, k) && !inList(c->minor, k); });
	}
	// Factions (rank, reputation there), reputation, bounty
	if (!w.pcRank.empty())
	{
		t += "\nFactions\n";
		for (auto& f : w.pcRank)
		{
			t += "  " + w.game.factionName(f.first) + ": " + w.game.rankName(f.first, f.second);
			auto rep = w.pcFacRep.find(f.first);
			if (rep != w.pcFacRep.end() && rep->second)
				t += "  (reputation " + std::to_string(rep->second) + ")";
			if (w.pcExpelled.count(f.first))
				t += "  - expelled";
			t += "\n";
		}
	}
	t += "\n" + w.game.gmstLabel("sreputation", "Reputation") + ": " + std::to_string(w.pcReputation) + "   " +
		w.game.gmstLabel("sbounty", "Bounty") + ": " + std::to_string(w.bounty) + "\n";
	return t;
}

void Session::drawStats()
{
	header(w.game.gmstLabel("sstats", "Stats"), dateText());
	uiPanel(4, 24, 312, 180);
	// Progress towards the next level: major and minor skill increases out of iLevelUpTotal
	const PlayerStats& s = w.stats;
	int total = levelUpTotal();
	uiText(8, 27, 0.45f, col::header, w.game.gmst("slevel", "Level") + " " + std::to_string(s.level));
	uiBar(76, 30, 190, 10, total > 0 ? fminf(1.0f, (float)s.levelProgress / total) : 0.0f, col::header);
	uiTextRight(312, 27, 0.42f, s.levelProgress >= total ? col::header : col::text,
		std::to_string(s.levelProgress) + "/" + std::to_string(total));
	// Tapping the level shows what the next level-up gives each attribute (OpenMW's level tooltip in the stats window:
	// every multiplier above 1, cut so the attribute can't pass 100)
	if (uiHit(4, 24, 312, 18))
	{
		statsBonusShown = !statsBonusShown;
		playSound(-1, "Menu Click");
	}
	if (statsBonusShown)
	{
		static const char* kAttrKeys[8] = { "sattributestrength", "sattributeintelligence", "sattributewillpower",
			"sattributeagility", "sattributespeed", "sattributeendurance", "sattributepersonality", "sattributeluck" };
		static const char* kAttr[8] = { "Strength", "Intelligence", "Willpower", "Agility", "Speed", "Endurance",
			"Personality", "Luck" };
		std::string t = "At the next level:\n";
		bool any = false;
		for (int a = 0; a < 8; a++)
		{
			int base = std::min(100, s.attrCreation[a] + s.attrBonus[a]);
			int mult = std::min(attrMultiplier(a), 100 - base);
			if (mult > 1)
			{
				t += w.game.gmstLabel(kAttrKeys[a], kAttr[a]) + " x" + std::to_string(mult) + "\n";
				any = true;
			}
		}
		if (!any)
			t += "no attribute above x1 yet\n";
		t += "\n(tap the level to close)";
		uiTextBox(scroll, 6, 44, 308, 158, t);
	}
	else
		uiTextBox(scroll, 6, 44, 308, 158, "(tap the level for the next level's attribute bonuses)\n" + statsText(*this, true));
	int b = buttonRow({ w.game.gmst("sclose", "Close") }, focus);
	if (b == 0 || (uiIn().down & KEY_B))
		closeScreen();
}
