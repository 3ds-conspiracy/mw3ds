// Spells known, diseases and curses on actors and the player
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
#include <unordered_set>
#include "mwbase/session.hpp"
#include "mwmechanics/magichelpers.hpp"

bool World::pcHasDisease(int type) const
{
	for (auto& id : stats.spells)
	{
		auto it = game.spells.find(lower(id));
		if (it != game.spells.end() && it->second.type == type)
			return true;
	}
	return false;
}

// Spells an NPC or creature has: what scripts gave it (AddSpell) and what its record has, less what they took away
bool World::actorHasSpell(const Ref& r, const std::string& id) const
{
	for (auto& s : r.spells)
		if (s == id)
			return true;
	for (auto& s : r.noSpells)
		if (s == id)
			return false;
	if (r.actor < 0)
		return false;
	const ActorDef& a = game.actors[r.actor];
	for (const std::vector<Atom>* list : { &a.spells, &a.combatSpells, &a.diseases })
		for (auto& s : *list)
			if (lower(s) == id)
				return true;
	return false;
}

// Whether one of their spells is of that type (2 blight, 3 common disease): GetBlightDisease / GetCommonDisease
bool World::actorHasSpellType(const Ref& r, int type) const
{
	std::vector<std::string> ids = r.spells;
	if (r.actor >= 0)
		for (const std::vector<Atom>* list : { &game.actors[r.actor].spells, &game.actors[r.actor].diseases })
			for (auto& s : *list)
				ids.push_back(lower(s));
	for (auto& id : ids)
	{
		auto sp = game.spells.find(id);
		if (sp != game.spells.end() && sp->second.type == type && actorHasSpell(r, id))
			return true;
	}
	return false;
}

bool World::pcHasCorprus() const
{
	if (corprusSince < -1.5f)
		return false;                       // Cure Corprus ended the effect (the disease stays on the list)
	for (auto& id : stats.spells)
	{
		auto it = game.spells.find(lower(id));
		if (it != game.spells.end())
			for (auto& e : it->second.effects)
				if (e.effect == 132)
					return true;
	}
	return false;
}

// ---- Spells

std::vector<std::string> Session::knownSpells()
{
	std::vector<std::string> out;
	auto add = [&](const std::string& id) {
		std::string l = lower(id);
		auto it = w.game.spells.find(l);
		if (it == w.game.spells.end() || (it->second.type != 0 && it->second.type != 5))
			return;                                       // abilities and diseases aren't cast
		for (auto& o : out)
			if (o == l)
				return;
		out.push_back(l);
	};
	if (const RaceDef* r = w.race())
		for (auto& s : r->spells)
			add(s);
	if (const BirthDef* b = w.birthsign())
		for (auto& s : b->spells)
			add(s);
	for (auto& s : w.stats.spells)
		add(s);
	return out;
}
