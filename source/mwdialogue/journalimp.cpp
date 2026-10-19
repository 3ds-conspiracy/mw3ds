// The journal: quest entries and indexes, and the topics log
#include "mwworld/worldimp.hpp"
#include "mwrender/objectpaging.hpp"
#include "components/files/datapath.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "extern/cjson/cJSON.h"
#include "platform/linear.hpp"
#include "platform/console.hpp"
extern "C" u32 __ctru_heap_size;
#include "platform/memstat.hpp"
#include <malloc.h>
#include "components/debug/debuglog.hpp"
#include "components/files/zfile.hpp"
#include "mwworld/worldhelpers.hpp"

void World::setJournal(const std::string& quest, int index)
{
	journalAdd(quest, index);
	journalIndex[lower(quest)] = index;
}

// OpenMW's Journal::addEntry / Quest::addEntry
bool World::journalAdd(const std::string& quest, int index)
{
	std::string q = lower(quest);
	int& now = journalIndex[q];
	auto jit = game.journals.find(q);
	const JournalEntry* e = nullptr;
	if (jit != game.journals.end())
		for (auto& je : jit->second.entries)
			if (je.index == index)
			{
				e = &je;
				break;
			}
	// no such stage: only the index moves (up)
	if (!e)
	{
		if (now < index)
			now = index;
		return false;
	}
	// written already: not again; a higher stage than the quest has is told
	for (auto& l : journal)
		if (l.index == index && l.quest == q)
		{
			if (now < index)
			{
				now = index;
				return true;
			}
			return false;
		}
	if (e->finished)
		questFinished.insert(q);
	else if (e->restart)
		questFinished.erase(q);
	if (e->restart && !jit->second.title.empty())
	{
		// every quest of the same name starts over
		const std::string& title = jit->second.title;
		for (auto it = questFinished.begin(); it != questFinished.end();)
		{
			auto other = game.journals.find(*it);
			if (other != game.journals.end() && lower(other->second.title) == lower(title))
				it = questFinished.erase(it);
			else
				++it;
		}
	}
	if (now < index)
		now = index;
	// nothing to read: nothing in the journal, nothing said
	if (e->text.empty())
		return false;
	journal.push_back({ q, index, e->text });
	return true;
}

void World::topicLogRemoveLast(const std::string& topic, const std::string& speaker)
{
	auto log = topicLog.find(topic);
	if (log == topicLog.end())
		return;
	const std::string prefix = speaker + ": ";
	for (size_t i = log->second.size(); i-- > 0;)
		if (log->second[i].compare(0, prefix.size(), prefix) == 0)
		{
			log->second.erase(log->second.begin() + i);
			break;
		}
	if (log->second.empty())
		topicLog.erase(log);
}

int World::getJournal(const std::string& quest) const
{
	auto it = journalIndex.find(lower(quest));
	return it == journalIndex.end() ? 0 : it->second;
}
