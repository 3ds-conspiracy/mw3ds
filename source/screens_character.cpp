// The character: journal, stats, character creation, level progress, death and the end
#include <cmath>
#include <cstdio>
#include <cstring>
#include "audio.h"
#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "screens.h"
#include "session.h"

void Session::drawJournal()
{
	header(w.game.gmst("sjournal", "Journal"));
	// Entries / Topics (L / R or tap), and Search
	const UiInput& in = uiIn();
	int tab = journalTab;
	if (in.down & (KEY_L | KEY_R)) tab = 1 - tab;
	if (uiButton(4, 24, 100, 18, "Entries", tab == 0)) tab = 0;
	if (uiButton(108, 24, 100, 18, w.game.gmst("stopics", "Topics"), tab == 1)) tab = 1;
	// Search (tap: the system keyboard): the topics whose name holds the text, the entries that do
	if (uiButton(212, 24, 104, 18, journalSearch.empty() ? "Search" : journalSearch, !journalSearch.empty()))
	{
		playSound(-1, "Menu Click");
		wantText = TEXT_SEARCH;
	}
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
	std::vector<std::string> topics, topicKeys;
	for (auto& t : w.topicLog)
	{
		topics.push_back(t.first);
		topicKeys.push_back(lower(t.first));
	}
	auto page = [&](const std::string& key, auto build, size_t skip) {
		long rev = (long)(std::hash<std::string>()(key + "|" + std::to_string(w.journal.size()) + "|"
			+ std::to_string(w.topicLog.size())) & 0x7fffffff);
		std::string text;
		std::vector<UiLink> links;
		if (rev != journalText.revision)
		{
			text = build();
			for (auto& m : dialogueFindKeywords(text, topicKeys))
				if (m.begin >= skip)
					links.push_back({ m.begin, m.end, m.keyword });
		}
		uiPanel(4, 44, 312, 160);
		int hit = uiLinkTextBox(journalText, rev, 6, 46, 308, 156, text, links);
		if (hit >= 0 && hit < (int)topics.size())
		{
			playSound(-1, "Menu Click");
			journalTab = 1;
			journalTopic = topics[hit];
			list2 = UiList();
			logf("journal: link to %s", journalTopic.c_str());
		}
	};
	if (journalTab == 0)
	{
		page("entries|" + find, [&]() {
			std::string text;
			for (auto it = w.journal.rbegin(); it != w.journal.rend(); ++it)
			{
				auto j = w.game.journals.find(it->quest);
				// the quest's name (its "quest name" INFO); Morrowind.esm has none, and the id is no title
				std::string title = j != w.game.journals.end() ? j->second.title : "";
				std::string entry = (title.empty() ? "" : title + "\n") + dialogueSubstitute(w, it->text, -1);
				if (found(entry))
					text += entry + "\n\n";
			}
			if (text.empty())
				text = find.empty() ? "No entries yet." : "Nothing found.";
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
	std::vector<std::string> labels;
	if (journalTab == 1 && !journalTopic.empty())
		labels.push_back(w.game.gmst("sback", "Back"));
	labels.push_back(w.game.gmst("sclose", "Close"));
	int b = buttonRow(labels, focus);
	if (journalTab == 1 && !journalTopic.empty() && (b == 0 || (in.down & KEY_B)))
		journalTopic.clear();
	else if (b == (int)labels.size() - 1 || (in.down & KEY_B))
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
	std::string t = "Name: " + s.name + "\nRace: " + race + (s.female ? " (female)" : " (male)") +
		"\nClass: " + cls + "\nSign: " + sign + (progress ? "" : "\nLevel: " + std::to_string(s.level)) + "\n\n";
	snprintf(buf, sizeof(buf), "Health %d   Magicka %d   Fatigue %d\n\n", (int)s.healthMax, (int)s.magickaMax, (int)s.fatigueMax);
	t += buf;
	for (int i = 0; i < 8; i++)
		t += attrName(w, i) + ": " + std::to_string(s.attributes[i]) + (i % 2 ? "\n" : "     ");
	auto section = [&](const char* title, auto pred) {
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
		section("Major Skills", [&](int k) { return inList(c->major, k); });
		section("Minor Skills", [&](int k) { return inList(c->minor, k); });
		section("Misc Skills", [&](int k) { return !inList(c->major, k) && !inList(c->minor, k); });
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
	t += "\nReputation: " + std::to_string(w.pcReputation) + "   Bounty: " + std::to_string(w.bounty) + "\n";
	return t;
}

void Session::drawStats()
{
	header("Stats", dateText());
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
	header("Class");
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
	std::string t = "Specialization: " + specName(w, c.specialization) + "\nFavored: " +
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
	header("You have died");
	uiPanel(4, 24, 312, 150);
	bool haveSave = deathHaveSave;
	uiTextBox(scroll, 10, 30, 300, 140, haveSave ? "Load your last save, or get up again where you fell."
		: "There is no save yet. Get up again where you fell.", 0.5f, false);
	std::vector<std::string> labels;
	if (haveSave)
		labels.push_back("Load last save");
	labels.push_back("Try again");
	int b = buttonRow(labels, focus);
	if (b >= 0 && labels[b] == "Load last save")
		wantReload = true;
	else if (b >= 0)
	{
		w.stats.health = w.stats.healthMax;
		w.stats.fatigue = w.stats.fatigueMax;
		playerDead = false;
		closeScreen();
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
