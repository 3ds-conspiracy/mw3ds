// A reference's saved state: what changed since the cell was read
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "extern/cjson/cJSON.h"
#include <vector>
#include <zlib.h>
#include "mwworld/worldimp.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include <cmath>
#include <malloc.h>
#include "mwstate/statehelpers.hpp"

cJSON* World::refState(int i) const
{
	const Ref& r = refs[i];
	cJSON* o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "c", cells[r.cell].file.c_str());
	cJSON_AddNumberToObject(o, "i", (double)(i - cells[r.cell].refBase));
	cJSON_AddStringToObject(o, "id", r.idLower.c_str());
	// (left out when they hold what applyRefState takes for missing: a save grows with every changed object, and
	// every door's autosave writes it all)
	if (!r.enabled)
		cJSON_AddNumberToObject(o, "enabled", 0);
	if (r.pickedUp)
		cJSON_AddNumberToObject(o, "taken", 1);
	if (r.lockLevel)
		cJSON_AddNumberToObject(o, "lock", r.lockLevel);
	if (r.disarmed)
		cJSON_AddNumberToObject(o, "disarmed", 1);
	if (r.doorTarget != 0.0f)
		cJSON_AddNumberToObject(o, "door", r.doorTarget);
	cJSON_AddNumberToObject(o, "disp", r.disposition);
	if (r.talkedToPC)
		cJSON_AddNumberToObject(o, "talked", 1);
	if (r.at >= 0)
		cJSON_AddStringToObject(o, "at", cells[r.at].file.c_str());
	// Objects scripts moved or turned (a raised portcullis, an opened Dwemer door)
	if (r.actor < 0 && r.moved)
	{
		cJSON_AddItemToObject(o, "opos", floats(r.pos, 3));
		cJSON_AddItemToObject(o, "orot", floats(r.rot, 3));
	}
	if (r.actor >= 0)
	{
		cJSON_AddNumberToObject(o, "health", r.health);
		cJSON_AddNumberToObject(o, "fatigue", r.fatigue);
		cJSON_AddNumberToObject(o, "gold", r.gold);
		if (r.hello >= 0)
			cJSON_AddNumberToObject(o, "hello", r.hello);
		if (r.friendlyHits)
			cJSON_AddNumberToObject(o, "fhits", r.friendlyHits);
		if (r.diedAt >= 0.0f)
			cJSON_AddNumberToObject(o, "died_at", r.diedAt);
		if (r.lastBarter > -999.0f)
			cJSON_AddNumberToObject(o, "bartered", r.lastBarter);
		if (r.dead)
			cJSON_AddNumberToObject(o, "dead", 1);
		if (r.aggressor)
			cJSON_AddNumberToObject(o, "aggressor", 1);
		if (r.fight >= 0)
			cJSON_AddNumberToObject(o, "fight", r.fight);
		if (r.reputation)
			cJSON_AddNumberToObject(o, "rep", r.reputation);
		if (r.level >= 0)
			cJSON_AddNumberToObject(o, "level", r.level);
		if (r.rank >= 0)
			cJSON_AddNumberToObject(o, "rank", r.rank);
		if (!r.statDelta.empty())
			cJSON_AddItemToObject(o, "stat_delta", floats(r.statDelta.data(), r.statDelta.size()));
		for (int k = 0; k < 2; k++)
		{
			const std::vector<std::string>& ids = k ? r.noSpells : r.spells;
			if (ids.empty())
				continue;
			cJSON* list = cJSON_AddArrayToObject(o, k ? "no_spells" : "spells");
			for (auto& id : ids)
				cJSON_AddItemToArray(list, cJSON_CreateString(id.c_str()));
		}
		if (r.moved)
		{
			cJSON_AddItemToObject(o, "pos", floats(r.pos, 3));
			cJSON_AddNumberToObject(o, "yaw", r.rot[2]);
		}
		if (r.aiPackage != AIPKG_NONE)
		{
			cJSON* ai = cJSON_AddObjectToObject(o, "ai");
			cJSON_AddNumberToObject(ai, "type", r.aiPackage);
			cJSON_AddStringToObject(ai, "target", r.aiTarget.c_str());
			cJSON_AddItemToObject(ai, "dest", floats(r.aiDest, 3));
			cJSON_AddNumberToObject(ai, "duration", r.aiDuration);
			cJSON_AddNumberToObject(ai, "start", r.aiStart);
			cJSON_AddNumberToObject(ai, "range", r.aiRange);
			cJSON_AddNumberToObject(ai, "done", r.aiDone);
			if (r.aiRepeat)
				cJSON_AddNumberToObject(ai, "repeat", 1);
			if (!r.aiCell.empty())
				cJSON_AddStringToObject(ai, "cell", r.aiCell.c_str());
		}
	}
	if (r.recType == REC_CONT || r.recType == REC_NPC_ || r.recType == REC_CREA)
	{
		cJSON* c = cJSON_AddArrayToObject(o, "contents");
		for (auto& it : r.contents)
		{
			cJSON* e = cJSON_CreateArray();
			cJSON_AddItemToArray(e, cJSON_CreateNumber(it.first));
			cJSON_AddItemToArray(e, cJSON_CreateString(it.second.c_str()));
			if (!it.plain())          // [count, id, condition, soul, charge]: a worn, filled or part-charged one
			{
				cJSON_AddItemToArray(e, cJSON_CreateNumber(it.condition));
				cJSON_AddItemToArray(e, cJSON_CreateString(it.soul.c_str()));
				cJSON_AddItemToArray(e, cJSON_CreateNumber(it.charge));
			}
			cJSON_AddItemToArray(c, e);
		}
	}
	return o;
}

// The state refState writes, hashed (FNV-1a): equal to baseHash while nothing about the object changed
u32 World::refHash(int i) const
{
	const Ref& r = refs[i];
	u32 h = 2166136261u;
	auto mix = [&](const void* p, size_t n) {
		const u8* b = (const u8*)p;
		for (size_t k = 0; k < n; k++)
			h = (h ^ b[k]) * 16777619u;
	};
	auto mixi = [&](int v) { mix(&v, sizeof(v)); };
	auto mixf = [&](float v) { mix(&v, sizeof(v)); };
	mixi(r.enabled);
	mixi(r.pickedUp);
	mixi(r.lockLevel);
	mixi(r.disarmed);
	mixf(r.doorTarget);
	mixi(r.disposition);
	mixi(r.talkedToPC);
	mixi(r.at);
	if (r.actor < 0 && r.moved)
	{
		mix(r.pos, sizeof(r.pos));
		mix(r.rot, sizeof(r.rot));
	}
	if (r.actor >= 0)
	{
		mixf(r.health);
		mixf(r.fatigue);
		mixi(r.gold);
		mixi(r.dead);
		mixi(r.aggressor);
		mixi(r.fight);
		mixi(r.reputation);
		mixi(r.level);
		mixi(r.rank);
		mix(r.statDelta.data(), r.statDelta.size() * sizeof(float));
		for (auto& id : r.spells)
			mix(id.data(), id.size());
		for (auto& id : r.noSpells)
			mix(id.data(), id.size());
		mixi(r.friendlyHits);
		mixf(r.diedAt);
		mixi(r.hello);
		mixi(r.moved);
		if (r.moved)
		{
			mix(r.pos, sizeof(r.pos));
			mixf(r.rot[2]);
		}
		mixi(r.aiPackage);
		mix(r.aiTarget.data(), r.aiTarget.size());
		mix(r.aiDest, sizeof(r.aiDest));
		mixf(r.aiDuration);
		mixf(r.aiStart);
		mixi(r.aiRange);
		mixi(r.aiDone);
	}
	for (auto& c : r.contents)
	{
		mixi(c.first);
		mix(c.second.data(), c.second.size());
		mixi(c.condition);
		mix(c.soul.data(), c.soul.size());
		mixf(c.charge);
	}
	return h;
}

void World::applyRefState(Ref& r, const cJSON* it)
{
	if (const cJSON* p = cJSON_GetObjectItem(it, "opos"))
	{
		readFloats(p, r.pos, 3);
		readFloats(cJSON_GetObjectItem(it, "orot"), r.rot, 3);
		r.moved = true;
	}
	r.enabled = num(it, "enabled", 1) != 0;
	r.pickedUp = num(it, "taken") != 0;
	r.lockLevel = (int)num(it, "lock");
	r.disarmed = num(it, "disarmed") != 0;
	r.doorTarget = r.doorAngle = num(it, "door");
	r.disposition = (int)num(it, "disp", r.disposition);
	r.talkedToPC = num(it, "talked") != 0;
	if (r.actor >= 0)
	{
		r.health = num(it, "health", r.health);
		r.fatigue = num(it, "fatigue", r.fatigue);
		r.gold = (int)num(it, "gold", r.gold);
		r.lastBarter = (float)num(it, "bartered", -1000.0);
		r.hello = (int)num(it, "hello", -1);
		r.friendlyHits = (int)num(it, "fhits", 0);
		r.diedAt = (float)num(it, "died_at", -1.0);
		r.dead = num(it, "dead") != 0;
		r.aggressor = num(it, "aggressor") != 0;
		r.fight = (int)num(it, "fight", -1);
		r.reputation = (int)num(it, "rep");
		r.level = (int)num(it, "level", -1);
		r.rank = (int)num(it, "rank", -1);
		if (r.actor >= 0)
		{
			// the record shows them as scripts left them (dialogue and combat read it)
			if (r.level >= 0)
				game.actors[r.actor].level = r.level;
			if (r.rank >= 0)
				game.actors[r.actor].rank = r.rank;
		}
		r.statDelta.clear();
		if (const cJSON* d = cJSON_GetObjectItem(it, "stat_delta"))
		{
			r.statDelta.assign(35, 0.0f);
			readFloats(d, r.statDelta.data(), 35);
		}
		for (int k = 0; k < 2; k++)
		{
			std::vector<std::string>& list = k ? r.noSpells : r.spells;
			list.clear();
			const cJSON* e;
			cJSON_ArrayForEach(e, cJSON_GetObjectItem(it, k ? "no_spells" : "spells"))
				if (cJSON_IsString(e))
					list.push_back(e->valuestring);
		}
		if (const cJSON* p = cJSON_GetObjectItem(it, "pos"))
		{
			readFloats(p, r.pos, 3);
			r.rot[2] = num(it, "yaw");
			r.moved = true;
		}
		if (const cJSON* ai = cJSON_GetObjectItem(it, "ai"))
		{
			r.aiPackage = (int)num(ai, "type", AIPKG_NONE);
			r.aiTarget = str(ai, "target");
			readFloats(cJSON_GetObjectItem(ai, "dest"), r.aiDest, 3);
			r.aiDuration = num(ai, "duration");
			r.aiStart = num(ai, "start");
			r.aiRange = (int)num(ai, "range", -1);
			r.aiDone = num(ai, "done") != 0;
			r.aiRepeat = num(ai, "repeat") != 0;
			r.aiCell = str(ai, "cell");
		}
	}
	r.fitBox();                    // moved: its box goes with it
	if (const cJSON* c = cJSON_GetObjectItem(it, "contents"))
	{
		r.contents.clear();
		const cJSON* e;
		cJSON_ArrayForEach(e, c)
		{
			r.contents.emplace_back(cJSON_GetArrayItem(e, 0)->valueint, cJSON_GetArrayItem(e, 1)->valuestring);
			if (cJSON_GetArraySize(e) >= 5)
			{
				r.contents.back().condition = cJSON_GetArrayItem(e, 2)->valueint;
				r.contents.back().soul = cJSON_GetArrayItem(e, 3)->valuestring;
				r.contents.back().charge = (float)cJSON_GetArrayItem(e, 4)->valuedouble;
			}
		}
	}
}

cJSON* World::scriptState(const ScriptInstance& s) const
{
	cJSON* o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "name", s.script->name.c_str());
	cJSON_AddNumberToObject(o, "ref", s.ref >= 0 && refs[s.ref].cell >= 0 ? s.ref - cells[refs[s.ref].cell].refBase : -1);
	if (s.ref >= 0 && refs[s.ref].cell >= 0)
	{
		cJSON_AddStringToObject(o, "ref_c", cells[refs[s.ref].cell].file.c_str());
		cJSON_AddStringToObject(o, "ref_id", refs[s.ref].idLower.c_str());
	}
	if (s.target >= 0 && s.target < (int)refs.size() && refs[s.target].cell >= 0)   // a global script's reference (cell file, index in it)
	{
		cJSON_AddStringToObject(o, "tgt_c", cells[refs[s.target].cell].file.c_str());
		cJSON_AddNumberToObject(o, "tgt_i", s.target - cells[refs[s.target].cell].refBase);
	}
	else if (s.target < 0 && !s.targetCell.empty())          // its cell is out of memory: as kept
	{
		cJSON_AddStringToObject(o, "tgt_c", s.targetCell.c_str());
		cJSON_AddNumberToObject(o, "tgt_i", s.targetIndex);
	}
	cJSON_AddStringToObject(o, "item", s.item.c_str());
	cJSON_AddNumberToObject(o, "running", s.running);
	cJSON_AddItemToObject(o, "locals", floats(s.locals.data(), s.locals.size()));
	return o;
}
