// The journal screen: entries by questline, pages
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
