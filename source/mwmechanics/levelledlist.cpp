// Leveled lists: candidates at the player's level, picks
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
#include "mwworld/esmstore.hpp"
#include "components/interpreter/interpreter.hpp"
#include "mwworld/esmhelpers.hpp"

// A leveled spot becomes that creature (the pick, or what a save says stood there)
bool World::applyLevPick(int ri, const std::string& pick)
{
	Ref& r = refs[ri];
	if (lower(pick) == r.idLower)
		return true;
	auto sp = game.spawns.find(lower(pick));
	if (sp == game.spawns.end() || sp->second.actor < 0)
		return false;
	bool live = r.cell >= 0 && cells[placeOf(ri)].live;
	if (live)
		detachActor(ri);
	r.id = game.actors[sp->second.actor].id;
	r.idLower = lower(r.id);
	refsChanged();
	r.actor = sp->second.actor;
	r.obj = game.object(r.id);
	r.contents.clear();
	fillContents(game, r, stats.level, (unsigned)ri * 2654435761u);
	initActorState(game, r);
	r.relib = true;
	if (live)
		attachSpawned(ri);
	return true;
}

void GameData::levCandidates(const LeveledList& l, int level, std::vector<int>& out)
{
	out.clear();
	int top = 0;
	for (auto& e : l.entries)
		if (e.first > top && e.first <= level)
			top = e.first;
	for (size_t i = 0; i < l.entries.size(); i++)
		if (l.entries[i].first <= level && (l.all || l.entries[i].first == top))
			out.push_back((int)i);
}

// One of the entries at or below the player's level, at random (nested lists followed)
std::string GameData::pickLeveled(const std::string& list, int level, unsigned* seed) const
{
	auto it = leveled.find(list);
	if (it == leveled.end())
		return list;
	auto roll = [&](int n) {
		if (!seed)
			return rand() % n;
		*seed = *seed * 1103515245u + 12345u;
		return (int)((*seed >> 16) % (unsigned)n);
	};
	const LeveledList& l = it->second;
	if (l.none > 0 && roll(100) < l.none)
		return "";
	std::vector<int> ok;
	levCandidates(l, level, ok);
	if (ok.empty())
		return "";
	const std::string& pick = l.entries[ok[roll((int)ok.size())]].second;
	return leveled.count(pick) ? pickLeveled(pick, level, seed) : pick;
}
