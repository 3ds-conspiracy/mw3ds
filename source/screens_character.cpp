// The character: journal, stats, character creation, level progress, death and the end
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "audio.h"
#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "screens.h"
#include "session.h"

// The questlines the journal's entries can be narrowed to, told apart by the start of the quest's id (A1_ / B5_ / C3_
// main quest, MG_ Mages Guild ...); every other quest is "Other"
static const struct { const char* name; const char* prefixes; } kQuestLines[] = {
	{ "Main Quest", "a1_ a2_ b1_ b2_ b2a b2b b2c b3_ b4_ b5_ b6_ b7_ b8_ c0_ c2_ c3_ cx_" },
	{ "Blades", "blades_" }, { "Fighters Guild", "fg_" }, { "Mages Guild", "mg_" }, { "Thieves Guild", "tg_" },
	{ "Morag Tong", "mt_" }, { "Imperial Legion", "il_" }, { "Imperial Cult", "ic" }, { "Tribunal Temple", "tt_" },
	{ "House Hlaalu", "hh_" }, { "House Redoran", "hr_" }, { "House Telvanni", "ht_" }, { "Daedric Quests", "da_" },
	{ "Vampires", "va_" }, { "Other", "" },
};
static const int kQuestLineCount = (int)(sizeof(kQuestLines) / sizeof(kQuestLines[0]));
static const int kJournalPageEntries = 10;

// The questline of a quest (lowercase id): the index in kQuestLines
static int questLine(const std::string& quest)
{
	for (int i = 0; i < kQuestLineCount - 1; i++)
	{
		const char* p = kQuestLines[i].prefixes;
		while (*p)
		{
			const char* e = strchr(p, ' ');
			size_t n = e ? (size_t)(e - p) : strlen(p);
			// ("ic" is followed by the quest's number: IC0_ ... IC30_)
			if (quest.compare(0, n, p, n) == 0 && (n != 2 || (quest.size() > 2 && (isdigit((u8)quest[2]) || quest[2] == '_'))))
				return i;
			p += n;
			while (*p == ' ')
				p++;
		}
	}
	return kQuestLineCount - 1;
}

void Session::drawJournal()
{
	header(w.game.gmst("sjournal", "Journal"));
	// Entries / Topics (L / R or tap), and Search
	const UiInput& in = uiIn();
	int tab = journalTab;
	if ((in.down & (KEY_L | KEY_R)) && !journalPicking) tab = 1 - tab;
	if (uiButton(4, 24, 100, 18, "Entries", tab == 0) && !journalPicking) tab = 0;
	if (uiButton(108, 24, 100, 18, w.game.gmst("stopics", "Topics"), tab == 1) && !journalPicking) tab = 1;
	// Search (tap: the system keyboard): the topics whose name holds the text, the entries that do
	// (A belongs to the text's links and the topic index, so neither the tabs nor Search answer it)
	bool searching = !journalSearch.empty();
	if (uiButton(212, 24, searching ? 82 : 104, 18, searching ? journalSearch : "Search", searching) && !(in.down & KEY_A)
		&& !journalPicking)
	{
		playSound(-1, "Menu Click");
		wantText = TEXT_SEARCH;
	}
	// one tap on the X empties the search again
	if (searching && uiButton(298, 24, 18, 18, "", false) && !journalPicking)
	{
		playSound(-1, "Menu Click");
		journalSearch.clear();
	}
	if (searching)       // (the letter drawn by hand: the glyph sits a pixel right of a centred X)
		uiText(298 + (18 - uiTextWidth("X", 0.5f)) / 2 - 1.0f, 24 + (18 - uiLineHeight(0.5f)) / 2, 0.5f, col::text, "X");
	if (tab != journalTab)
	{
		journalTab = tab;
		list2 = UiList();
		journalTopic.clear();
	}
	std::string find = lower(journalSearch);
	auto found = [&](const std::string& text) { return find.empty() || lower(text).find(find) != std::string::npos; };
	// The topics the journal has are links in its text, and open that topic (OpenMW's JournalBooks); the page is
	// built and laid out again only when what it shows changes
	std::vector<std::string> topics, topicKeys, topicIds;
	for (auto& t : w.topicLog)
	{
		topics.push_back(t.first);
		topicKeys.push_back(lower(w.game.topicKeyword(t.first)));
		topicIds.push_back(lower(t.first));
	}
	auto page = [&](const std::string& key, auto build, size_t skip) {
		long rev = (long)(std::hash<std::string>()(key + "|" + std::to_string(w.journal.size()) + "|"
			+ std::to_string(w.topicLog.size())) & 0x7fffffff);
		std::string text;
		std::vector<UiLink> links;
		if (rev != journalText.revision)
		{
			HyperText h = dialogueHyperText(w.game, build(), topicKeys, topicIds);
			text = h.text;
			for (auto& m : h.links)
				if (m.begin >= skip)
					links.push_back({ m.begin, m.end, m.keyword });
		}
		uiPanel(4, 44, 312, 160);
		int hit = uiLinkTextBox(journalText, rev, 6, 46, 308, 156, text, links, 0.5f, !journalPicking);
		if (hit >= 0 && hit < (int)topics.size() && !journalPicking)
		{
			playSound(-1, "Menu Click");
			journalTab = 1;
			journalTopic = topics[hit];
			list2 = UiList();
			logf("journal: link to %s", journalTopic.c_str());
		}
	};
	auto entryText = [&](const JournalLine& l) {
		auto j = w.game.journals.find(l.quest);
		// the quest's name (its "quest name" INFO); Morrowind.esm has none, and the id is no title
		std::string title = j != w.game.journals.end() ? j->second.title : "";
		return (title.empty() ? "" : title + "\n") + dialogueSubstitute(w, l.text, -1);
	};
	int pages = 1;
	if (journalTab == 0)
	{
		// ZL / ZR: the questline before / after, past those with no entries (here, before the list below is
		// built, so the page shows the new one at once)
		if (!journalPicking && journalCatCount.size() == (size_t)kQuestLineCount + 1 && (in.down & (KEY_ZL | KEY_ZR)))
		{
			do
				journalCat = (journalCat + ((in.down & KEY_ZR) ? 1 : kQuestLineCount)) % (kQuestLineCount + 1);
			while (journalCat != 0 && journalCatCount[journalCat] == 0);
			journalPage = 0;
		}
		// The entries in the questline and the search, newest first, worked out again only when one of them changes
		long key = (long)(std::hash<std::string>()(std::to_string(journalCat) + "|" + find + "|"
			+ std::to_string(w.journal.size())) & 0x7fffffff);
		if (key != journalShownKey)
		{
			journalShownKey = key;
			journalShown.clear();
			journalCatCount.assign(kQuestLineCount + 1, 0);
			for (int i = (int)w.journal.size() - 1; i >= 0; i--)
			{
				int line = questLine(lower(w.journal[i].quest));
				if (!find.empty() && !found(entryText(w.journal[i])))
					continue;
				journalCatCount[0]++;
				journalCatCount[line + 1]++;
				if (journalCat == 0 || journalCat == line + 1)
					journalShown.push_back(i);
			}
		}
		pages = std::max(1, ((int)journalShown.size() + kJournalPageEntries - 1) / kJournalPageEntries);
		// Y / X: the page before / after (the D-pad belongs to the text's links)
		if (!journalPicking && (in.down & KEY_Y) && journalPage > 0)
			journalPage--;
		if (!journalPicking && (in.down & KEY_X) && journalPage < pages - 1)
			journalPage++;
		journalPage = std::max(0, std::min(journalPage, pages - 1));
		page("entries|" + std::to_string(journalCat) + "|" + std::to_string(journalPage) + "|" + find, [&]() {
			std::string text;
			for (int k = journalPage * kJournalPageEntries; k < (int)journalShown.size() && k < (journalPage + 1) * kJournalPageEntries; k++)
				text += entryText(w.journal[journalShown[k]]) + "\n\n";
			if (text.empty())
				text = !find.empty() ? "Nothing found." : w.journal.empty() ? "No entries yet." : "No entries for this questline.";
			return text;
		}, 0);
	}
	else if (journalTopic.empty())
	{
		// the topics heard about, alphabetically
		std::vector<std::string> names;
		for (auto& t : topics)
			if (found(t))
				names.push_back(t);
		if (names.empty())
			uiTextCentered(160, 110, 0.45f, col::textDim, topics.empty() ? "No topics yet." : "Nothing found.");
		int pick = uiList(list2, 4, 44, 312, 160, names, 0.45f, true);
		if (pick >= 0 && pick < (int)names.size())
			journalTopic = names[pick];
	}
	else
	{
		// the topic's name above what was said about it; the name itself is no link
		page("topic|" + journalTopic, [&]() {
			std::string text = journalTopic + "\n\n";
			for (auto& l : w.topicLog[journalTopic])
				text += l + "\n\n";
			return text;
		}, journalTopic.size());
	}
	// The questline list, over the page: every questline with entries, how many each
	if (journalTab == 0 && journalPicking)
	{
		std::vector<std::string> rows;
		std::vector<int> cats;
		for (int c = 0; c <= kQuestLineCount; c++)
			if (c == 0 || journalCatCount[c] > 0)
			{
				rows.push_back(std::string(c == 0 ? "All Quests" : kQuestLines[c - 1].name) + "  (" + std::to_string(journalCatCount[c]) + ")");
				cats.push_back(c);
			}
		uiRect(4, 44, 312, 160, C2D_Color32(0, 0, 0, 255));      // (the page under it hidden)
		uiFrame(4, 44, 312, 160);
		int pick = uiList(journalCatList, 6, 46, 308, 156, rows, 0.5f, true);
		if (pick >= 0 && pick < (int)cats.size())
		{
			playSound(-1, "Menu Click");
			journalCat = cats[pick];
			journalPage = 0;
			journalPicking = false;
		}
		else if (in.down & KEY_B)
			journalPicking = false;
		uiConsume(KEY_A | KEY_B | KEY_UP | KEY_DOWN);
	}
	// the buttons below are for touch and B: the D-pad and A choose and open the links above
	if (journalTab == 1 && !journalTopic.empty())
	{
		int b = -1;
		std::string labels[2] = { w.game.gmst("sback", "Back"), w.game.gmst("sclose", "Close") };
		for (int i = 0; i < 2; i++)
			if (uiButton(4 + i * 158, 208, 154, 28, labels[i], false))
				b = i;
		if (b == 0 || (in.down & KEY_B))
			journalTopic.clear();
		else if (b == 1)
			closeScreen();
		return;
	}
	if (journalTab == 0)
	{
		// The questline (tap: the list of them), the page before / after, Close
		std::string cat = journalCat == 0 ? "All Quests" : kQuestLines[journalCat - 1].name;
		if (uiButton(4, 208, 118, 28, cat, journalPicking))
		{
			playSound(-1, "Menu Click");
			journalPicking = !journalPicking;
			// (the list opens on the questline shown)
			journalCatList = UiList();
			for (int c = 1; c <= journalCat; c++)
				journalCatList.selected += journalCatCount[c] > 0 || c == journalCat;
		}
		auto arrow = [&](float bx, bool right, bool enabled) {
			bool hit = uiButton(bx, 208, 32, 28, "", false, enabled);
			u32 c = enabled ? col::text : col::textDim;
			for (int k = 0; k < 5; k++)     // a small triangle, drawn by hand as the map's - and +
				uiRect(right ? bx + 13 + k : bx + 18 - k, 222 - (4 - k), 1, (4 - k) * 2 + 1, c);
			return hit;
		};
		// (the pages are the entries': the questline list over them has none, it scrolls)
		if (!journalPicking)
		{
			if (arrow(126, false, journalPage > 0))
			{
				playSound(-1, "Menu Click");
				journalPage--;
			}
			uiTextCentered(180, 208 + (28 - uiLineHeight(0.45f)) / 2, 0.45f, col::text,
				std::to_string(journalPage + 1) + "/" + std::to_string(pages));
			if (arrow(202, true, journalPage < pages - 1))
			{
				playSound(-1, "Menu Click");
				journalPage++;
			}
		}
		if ((uiButton(238, 208, 78, 28, w.game.gmst("sclose", "Close"), false) || (in.down & KEY_B)) && !journalPicking)
			closeScreen();
		return;
	}
	if (uiButton(4, 208, 312, 28, w.game.gmst("sclose", "Close"), false) || (in.down & KEY_B))
		closeScreen();
}

static std::string statsText(Session& ss, bool progress)
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
	uiTextBox(scroll, 6, 44, 308, 158, statsText(*this, true));
	int b = buttonRow({ w.game.gmst("sclose", "Close") }, focus);
	if (b == 0 || (uiIn().down & KEY_B))
		closeScreen();
}

// ---- character creation

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
	if (uiButton(124, 196, 192, 36, w.game.gmst("sok", "OK"), true) || (pick >= 0 && (in.down & KEY_A)))
	{
		w.stats.race = r->id;
		w.recomputeStats();
		logf("chargen: race %s %s", r->id.c_str(), w.stats.female ? "female" : "male");
		closeScreen();
	}
}

void Session::drawClassMethod()
{
	header(w.game.gmstLabel("sclass", "Class"));
	uiPanel(4, 28, 312, 60);
	uiTextBox(scroll, 6, 34, 308, 50, "Socucius Ergalla needs to record your class. How would you like to do this?", 0.5f, false);
	const UiInput& in = uiIn();
	if (in.down & KEY_DOWN) focus = (focus + 1) % 2;
	if (in.down & KEY_UP) focus = (focus + 1) % 2;
	if (uiButton(20, 104, 280, 40, w.game.gmst("sclasschoicemenu1", "Answer his questions"), focus == 0))
	{
		openScreen(SCR_CLASS_QUIZ);
		quizIndex = 0;
		quizScroll = UiScroll();
		quizCounts[0] = quizCounts[1] = quizCounts[2] = 0;
		if (!w.game.quiz.empty())
			say(-1, w.game.quiz[0].sound, "");
	}
	else if (uiButton(20, 154, 280, 40, w.game.gmst("sclasschoicemenu2", "Pick from the class list"), focus == 1))
		openScreen(SCR_CLASS_LIST);
}

static std::string classText(World& w, const ClassDef& c)
{
	std::string t = w.game.gmstLabel("sspecialization", "Specialization") + ": " + specName(w, c.specialization) + "\nFavored: " +
		attrName(w, c.attributes[0]) + ", " + attrName(w, c.attributes[1]) + "\n\nMajor skills:\n";
	for (int i = 0; i < 5; i++) t += "  " + skillName(w, c.major[i]) + "\n";
	t += "Minor skills:\n";
	for (int i = 0; i < 5; i++) t += "  " + skillName(w, c.minor[i]) + "\n";
	return t + "\n" + c.desc;
}

void Session::drawClassList()
{
	header("Choose your class");
	std::vector<const ClassDef*> classes;
	std::vector<std::string> rows;
	for (auto& c : w.game.classes)
		if (c.playable)
		{
			classes.push_back(&c);
			rows.push_back(c.name);
		}
	int pick = uiList(list, 4, 24, 116, 212, rows);
	const ClassDef* c = classes[list.selected < (int)classes.size() ? list.selected : 0];
	uiPanel(124, 24, 192, 168);
	uiTextBox(scroll, 126, 26, 188, 164, classText(w, *c), 0.45f, false);
	if (uiButton(124, 196, 192, 36, w.game.gmst("sok", "OK"), true) || (pick >= 0 && (uiIn().down & KEY_A)))
	{
		w.stats.cls = c->id;
		w.recomputeStats();
		logf("chargen: class %s", c->id.c_str());
		closeScreen();
	}
}

// Class from the question answers (combat / magic / stealth counts), as the original engine picks it
static std::string generatedClass(const int* n)
{
	int combat = n[0], magic = n[1], stealth = n[2];
	if (combat > 7) return "Warrior";
	if (magic > 7) return "Mage";
	if (stealth > 7) return "Thief";
	switch (combat)
	{
	case 4: return "Rogue";
	case 5: return stealth == 3 ? "Scout" : "Archer";
	case 6: return stealth == 1 ? "Barbarian" : stealth == 3 ? "Crusader" : "Knight";
	case 7: return "Warrior";
	}
	switch (magic)
	{
	case 4: return "Spellsword";
	case 5: return "Witchhunter";
	case 6: return combat == 2 ? "Sorcerer" : combat == 3 ? "Healer" : "Battlemage";
	case 7: return "Mage";
	}
	switch (stealth)
	{
	case 3: return magic == 3 ? "Bard" : "Warrior";
	case 5: return magic == 3 ? "Monk" : "Pilgrim";
	case 6: return magic == 1 ? "Agent" : magic == 3 ? "Assassin" : "Acrobat";
	case 7: return "Thief";
	}
	return "Warrior";
}

void Session::drawClassQuiz()
{
	if (quizIndex >= (int)w.game.quiz.size())
	{
		pickedClass = generatedClass(quizCounts);
		openScreen(SCR_CLASS_RESULT);
		focus = 1;    // OK
		return;
	}
	const QuizQuestion& q = w.game.quiz[quizIndex];
	header("Question " + std::to_string(quizIndex + 1) + " of " + std::to_string(w.game.quiz.size()));
	// The question and its three answers; most are taller than the screen, so the page scrolls (drag it, or the
	// circle pad), and the D-pad's choice is scrolled into view
	const float top = 22.0f, bottom = 240.0f;
	std::vector<std::string> ql = uiWrap(q.question, 300, 0.45f);
	float lh = uiLineHeight(0.45f), lha = uiLineHeight(0.42f);
	std::vector<std::string> al[3];
	float ay[3], ah[3];
	float content = 4 + ql.size() * lh + 4;
	for (int a = 0; a < 3; a++)
	{
		al[a] = uiWrap(q.answers[a], 288, 0.42f);
		ay[a] = content;
		ah[a] = al[a].size() * lha + 6;
		content += ah[a] + 4;
	}
	const UiInput& in = uiIn();
	bool moved = in.down & (KEY_DOWN | KEY_UP);
	if (in.down & KEY_DOWN) focus = (focus + 1) % 3;
	if (in.down & KEY_UP) focus = (focus + 2) % 3;
	float& sc = quizScroll.scroll;
	if (in.touching && in.touchY >= top)
		sc -= in.dragDY;
	sc += uiStickScroll();
	if (moved)
		sc = fminf(fmaxf(sc, ay[focus] + ah[focus] - (bottom - top) + 2), ay[focus] - 2);
	sc = fmaxf(0.0f, fminf(sc, fmaxf(0.0f, content - (bottom - top))));
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - bottom), 0, (u32)(240 - top), 320);
	float y0 = top - sc;
	for (size_t i = 0; i < ql.size(); i++)
		uiText(10, y0 + 4 + i * lh, 0.45f, col::header, ql[i]);
	int answer = -1;
	for (int a = 0; a < 3; a++)
	{
		float y = y0 + ay[a], h = ah[a];
		uiRect(4, y, 312, h, a == focus ? col::border : col::textDim);
		uiRect(5, y + 1, 310, h - 2, a == focus ? col::select : col::panelLight);
		for (size_t i = 0; i < al[a].size(); i++)
			uiText(12, y + 3 + i * lha, 0.42f, col::text, al[a][i]);
		if (uiHit(4, fmaxf(y, top), 312, fminf(y + h, bottom) - fmaxf(y, top)))
			answer = a;
		if (a == focus)
			quizBottom = y + h;
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	if (in.down & KEY_A)
		answer = focus;
	if (answer >= 0)
	{
		quizCounts[answer]++;
		quizIndex++;
		focus = 0;
		quizScroll = UiScroll();
		playSound(-1, "Menu Click");
		if (quizIndex < (int)w.game.quiz.size())
			say(-1, w.game.quiz[quizIndex].sound, "");
	}
}

void Session::drawClassResult()
{
	const ClassDef* c = nullptr;
	for (auto& k : w.game.classes)
		if (lower(k.id) == lower(pickedClass))
			c = &k;
	header("Your class");
	if (!c)
	{
		openScreen(SCR_CLASS_LIST);
		return;
	}
	uiPanel(4, 24, 312, 178);
	uiTextBox(scroll, 6, 26, 308, 174, "Based on your answers, you are a " + c->name + ".\n\n" + classText(w, *c), 0.45f, false);
	int b = buttonRow({ w.game.gmst("sback", "Back"), w.game.gmst("sok", "OK") }, focus);
	if (b == 1)
	{
		w.stats.cls = c->id;
		w.recomputeStats();
		logf("chargen: class %s (questions)", c->id.c_str());
		closeScreen();
	}
	else if (b == 0)
		openScreen(SCR_CLASS_METHOD);
}

void Session::drawBirth()
{
	header("Choose your birthsign");
	std::vector<std::string> rows;
	for (auto& b : w.game.birthsigns)
		rows.push_back(b.name);
	int pick = uiList(list, 4, 24, 116, 212, rows);
	const BirthDef& b = w.game.birthsigns[list.selected < (int)rows.size() ? list.selected : 0];
	std::string text = b.desc + "\n\n" + w.game.gmst("sbirthsignmenu1", "Abilities:") + "\n";
	for (auto& id : b.spells)
	{
		auto it = w.game.spells.find(lower(id));
		if (it == w.game.spells.end())
			continue;
		const char* kind = it->second.type == 1 ? "Ability" : it->second.type == 5 ? "Power" : "Spell";
		text += "  " + it->second.name + " (" + kind + ")\n";
	}
	uiPanel(124, 24, 192, 168);
	uiTextBox(scroll, 126, 26, 188, 164, text, 0.45f, false);
	if (uiButton(124, 196, 192, 36, w.game.gmst("sok", "OK"), true) || (pick >= 0 && (uiIn().down & KEY_A)))
	{
		w.stats.birthsign = b.id;
		w.recomputeStats();
		logf("chargen: birthsign %s", b.id.c_str());
		closeScreen();
	}
}

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
	uiPanel(4, 24, 312, 178);
	uiTextBox(scroll, 6, 26, 308, 174, statsText(*this, false), 0.45f, true);
	if (buttonRow({ w.game.gmst("sok", "OK") }, focus) == 0)
	{
		logf("chargen: review accepted");
		givePlayerStartSpells();
		closeScreen();
	}
}

void Session::drawDeath()
{
	// Morrowind and OpenMW: death leads to loading a game, never a getting up again where one fell
	header("You have died");
	uiPanel(4, 24, 312, 150);
	bool haveSave = !deathSave.empty();
	uiTextBox(scroll, 10, 30, 300, 140, haveSave ? "Load your last save, or choose another."
		: deathAnySave ? "Choose a save to load." : "There is no save yet. Start over.", 0.5f, false);
	std::vector<std::string> labels;
	if (haveSave)
		labels.push_back("Load last save");
	if (deathAnySave)
		labels.push_back("Choose a save");
	else
		labels.push_back("Start over");
	int b = buttonRow(labels, focus);
	if (b >= 0 && labels[b] == "Load last save")
	{
		reloadPath = deathSave;
		wantReload = true;
	}
	else if (b >= 0 && labels[b] == "Choose a save")
	{
		saveList = savesList(dataDir);
		list.selected = saveList.empty() ? -1 : 0;
		openScreen(SCR_SAVES);
	}
	else if (b >= 0)
	{
		reloadPath = kAutosavePath;     // (none there: a new game)
		wantReload = true;
	}
}

void Session::drawEnd()
{
	header("Released");
	std::string text = "You step out of the Census and Excise Office, a free " +
		std::string(w.stats.female ? "woman" : "man") + " in Vvardenfell.\n\n" +
		statsText(*this, false);
	uiPanel(4, 24, 312, 178);
	uiTextBox(scroll, 6, 26, 308, 174, text, 0.45f, true);
	if (buttonRow({ "Keep exploring" }, focus) == 0)
		closeScreen();
}
