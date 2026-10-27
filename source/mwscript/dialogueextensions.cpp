// Script functions: journal, topics, choices, greetings, reputation
#include "components/interpreter/interpreter.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "mwworld/worldimp.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

std::optional<float> dialogueExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	case F_JOURNAL:
	{
		std::string quest = argStr(c, n, 0);
		int idx = (int)argNum(c, n, 1);
		// Journal writes the entry but never lowers the quest's index (SetJournalIndex does): Gilvas Barelo's
		// "Journal A2_4_MiloGone 50 / ... 49" leaves it at 50 with both entries. The message comes only with
		// an entry that has text (or a stage already written that raises the index)
		if (w.journalAdd(quest, idx))
			c.host.notify(w.game.gmst("sjournalentry", "Your journal has been updated."));
		return 0.0f;
	}
	case F_GETJOURNALINDEX: return (float)w.getJournal(argStr(c, n, 0));
	case F_SETJOURNALINDEX: w.journalIndex[lower(argStr(c, n, 0))] = (int)argNum(c, n, 1); return 0.0f;
	case F_ADDTOPIC: w.knownTopics.insert(lower(argStr(c, n, 0))); return 0.0f;
	case F_GOODBYE: c.host.dialogueGoodbye(); return 0.0f;
	case F_CHOICE:
	{
		std::vector<std::pair<std::string, int>> choices;
		// a question with no number after it answers 1
		for (size_t i = 0; i < n.kids.size(); i += 2)
			choices.emplace_back(argStr(c, n, i), i + 1 < n.kids.size() ? (int)argNum(c, n, i + 1) : 1);
		c.host.dialogueChoice(choices);
		return 0.0f;
	}
	// (not to a werewolf, unless the speaker's script has a local allowwerewolfforcegreeting: Bloodmoon's hunters
	// and the Skaal talk to the wolf; OpenMW's OpForceGreeting)
	case F_FORCEGREETING:
		if (ref >= 0 && (!w.stats.werewolf || (r && r->script >= 0 && w.scripts[r->script].local("allowwerewolfforcegreeting"))))
			c.host.forceGreeting(ref);
		return 0.0f;
	// An actor's reputation is its record's (r->reputation is the script's change on top of it)
	case F_MODREPUTATION:
		if (onPlayer || !r)
			w.pcReputation += (int)argNum(c, n, 0);
		else
			r->reputation += (int)argNum(c, n, 0);
		return 0.0f;
	case F_SETREPUTATION:
		if (onPlayer || !r)
			w.pcReputation = (int)argNum(c, n, 0);
		else
			r->reputation = (int)argNum(c, n, 0) - (r->actor >= 0 ? w.game.actors[r->actor].reputation : 0);
		return 0.0f;
	case F_GETREPUTATION:
		if (onPlayer || !r)
			return (float)w.pcReputation;
		return (float)(r->reputation + (r->actor >= 0 ? w.game.actors[r->actor].reputation : 0));
	case F_CLEARINFOACTOR:
		// (OpenMW) takes the actor's last answer to the topic out of the journal's Topics index
		if (r && w.currentInfo >= 0)
			w.topicLogRemoveLast(w.currentTopic, r->actor >= 0 ? w.game.actors[r->actor].name : r->id.str());
		return 0.0f;
	// FillJournal: every quest's every entry, and every topic known (OpenMW also fills each topic's page with all its
	// answers: left out here)
	case F_FILLJOURNAL:
		for (auto& q : w.game.journals)
			for (auto& e : q.second.entries)
				w.journalAdd(q.first, e.index);
		for (auto& t : w.game.topics)
			if (t.type == 0)
				w.knownTopics.insert(t.lower);
		return 0.0f;
	default:
		return std::nullopt;
	}
}
