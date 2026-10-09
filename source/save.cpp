// Save games: one JSON file with everything that changes while playing. References are stored
// by cell file + index with their id, so a save made before the data was re-converted skips references
// that no longer line up instead of applying their state to the wrong object. Only objects that
// changed are written; cells not in memory keep theirs in World::cellStates (the same items).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "log.h"
#include "saves.h"
#include "zfile.h"
#include <vector>
#include <zlib.h>
#include "world.h"

static const int kSaveVersion = 1;

static cJSON* floats(const float* v, int n)
{
	return cJSON_CreateFloatArray(v, n);
}

static void readFloats(const cJSON* a, float* out, int n)
{
	for (int i = 0; i < n && i < cJSON_GetArraySize(a); i++)
		out[i] = (float)cJSON_GetArrayItem(a, i)->valuedouble;
}

static std::string str(const cJSON* o, const char* key)
{
	const cJSON* v = cJSON_GetObjectItem(o, key);
	return cJSON_IsString(v) ? v->valuestring : "";
}

static double num(const cJSON* o, const char* key, double fallback = 0.0)
{
	const cJSON* v = cJSON_GetObjectItem(o, key);
	return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

// ---- objects and scripts

cJSON* World::refState(int i) const
{
	const Ref& r = refs[i];
	cJSON* o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "c", cells[r.cell].file.c_str());
	cJSON_AddNumberToObject(o, "i", (double)(i - cells[r.cell].refBase));
	cJSON_AddStringToObject(o, "id", r.idLower.c_str());
	cJSON_AddNumberToObject(o, "enabled", r.enabled);
	cJSON_AddNumberToObject(o, "taken", r.pickedUp);
	cJSON_AddNumberToObject(o, "lock", r.lockLevel);
	if (r.disarmed)
		cJSON_AddNumberToObject(o, "disarmed", 1);
	cJSON_AddNumberToObject(o, "door", r.doorTarget);
	cJSON_AddNumberToObject(o, "disp", r.disposition);
	cJSON_AddNumberToObject(o, "talked", r.talkedToPC);
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
		cJSON_AddNumberToObject(o, "dead", r.dead);
		cJSON_AddNumberToObject(o, "aggressor", r.aggressor);
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
	if (r.type == "CONT" || r.type == "NPC_" || r.type == "CREA")
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

// A cell's changed objects and its objects' scripts that hold anything, as text ("" when nothing)
std::string World::cellStateText(int c) const
{
	const LevelCell& lc = cells[c];
	int first = lc.refBase, last = lc.refBase + lc.refCount, n = 0;
	cJSON* st = cJSON_CreateObject();
	cJSON* rs = cJSON_AddArrayToObject(st, "refs");
	cJSON* sc = cJSON_AddArrayToObject(st, "scripts");
	for (int i = first; i < last; i++)
		if (refHash(i) != refs[i].baseHash)
		{
			cJSON_AddItemToArray(rs, refState(i));
			n++;
		}
	for (auto& s : scripts)
	{
		if (s.ref < first || s.ref >= last || !s.item.empty())
			continue;
		bool set = !s.running;
		for (float v : s.locals)
			set |= v != 0.0f;
		if (set)
		{
			cJSON_AddItemToArray(sc, scriptState(s));
			n++;
		}
	}
	std::string out;
	if (n)
	{
		char* t = cJSON_PrintUnformatted(st);
		out = t;
		free(t);
	}
	cJSON_Delete(st);
	return out;
}

// What changed in a cell before (a save, or its objects leaving memory), onto its freshly read objects
void World::applyCellState(int c)
{
	LevelCell& lc = cells[c];
	auto found = cellStates.find(lc.file);
	if (found == cellStates.end())
		return;
	cJSON* st = cJSON_Parse(found->second.c_str());
	cellStates.erase(found);
	if (!st)
		return;
	auto at = [&](const cJSON* it, const char* key, const char* idKey) {
		int local = (int)num(it, key, -1);
		if (local < 0 || local >= lc.refCount)
			return -1;
		int i = lc.refBase + local;
		// a leveled spot holds what the save says stood there
		if (refs[i].idLower != str(it, idKey) && !refs[i].levList.empty())
			applyLevPick(i, str(it, idKey));
		return refs[i].idLower == str(it, idKey) ? i : -1;   // data re-converted since: skip it
	};
	const cJSON* it;
	int skipped = 0;
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(st, "refs"))
	{
		int i = at(it, "i", "id");
		if (i < 0)
			skipped++;
		else
		{
			applyRefState(refs[i], it);
			// Moved to another cell: drawn there from its mesh library
			std::string at = str(it, "at");
			int ac = at.empty() ? -1 : cellIndex(at);
			if (ac >= 0 && ac != refs[i].cell)
			{
				refs[i].at = ac;
				spawned.push_back(i);
				if (cells[ac].live && refs[i].actor >= 0)
					attachSpawned(i);
			}
		}
	}
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(st, "scripts"))
	{
		int i = at(it, "ref", "ref_id");
		const Script* s = game.script(lower(str(it, "name")));
		if (i < 0 || !s)
		{
			skipped++;
			continue;
		}
		if (refs[i].script < 0 || scripts[refs[i].script].script != s)
		{
			ScriptInstance inst;
			inst.script = s;
			inst.ref = i;
			refs[i].script = scripts.size();
			scripts.push_back(std::move(inst));
		}
		ScriptInstance& inst = scripts[refs[i].script];
		inst.running = num(it, "running", 1) != 0;
		inst.locals.assign(s->localNames.size(), 0.0f);
		readFloats(cJSON_GetObjectItem(it, "locals"), inst.locals.data(), inst.locals.size());
	}
	cJSON_Delete(st);
	if (skipped)
		logf("save: %s: %d saved entries no longer match the data", lc.file.c_str(), skipped);
}

bool World::saveGame(const char* path) const
{
	cJSON* root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "version", kSaveVersion);
	cJSON_AddStringToObject(root, "cell", cells[current].file.c_str());     // unique, unlike outdoor names
	cJSON_AddItemToObject(root, "feet", floats(player.feet, 3));
	cJSON_AddNumberToObject(root, "yaw", player.yaw);
	cJSON_AddNumberToObject(root, "pitch", player.pitch);
	cJSON_AddNumberToObject(root, "time", time);
	cJSON_AddNumberToObject(root, "hour", gameHour);
	cJSON_AddNumberToObject(root, "controls", controlsEnabled);
	cJSON_AddNumberToObject(root, "jumping", jumpingEnabled);
	cJSON_AddNumberToObject(root, "fighting", fightingEnabled);
	cJSON_AddNumberToObject(root, "magic", magicEnabled);
	cJSON_AddNumberToObject(root, "controlsoff", controlsOff);
	cJSON_AddNumberToObject(root, "menus", menusEnabled);

	cJSON* st = cJSON_AddObjectToObject(root, "stats");
	cJSON_AddStringToObject(st, "name", stats.name.c_str());
	cJSON_AddStringToObject(st, "race", stats.race.c_str());
	cJSON_AddStringToObject(st, "class", stats.cls.c_str());
	cJSON_AddStringToObject(st, "birthsign", stats.birthsign.c_str());
	cJSON_AddNumberToObject(st, "female", stats.female);
	float bars[6] = { stats.health, stats.healthMax, stats.magicka, stats.magickaMax, stats.fatigue, stats.fatigueMax };
	cJSON_AddItemToObject(st, "bars", floats(bars, 6));
	cJSON_AddNumberToObject(st, "level", stats.level);
	cJSON_AddNumberToObject(st, "level_progress", stats.levelProgress);
	cJSON_AddNumberToObject(st, "health_bonus", stats.healthBonus);
	cJSON_AddItemToObject(st, "skill_progress", floats(stats.skillProgress, 27));
	cJSON_AddItemToObject(st, "skill_bonus", cJSON_CreateIntArray(stats.skillBonus, 27));
	cJSON_AddItemToObject(st, "attr_bonus", cJSON_CreateIntArray(stats.attrBonus, 8));
	cJSON_AddItemToObject(st, "attr_ups", cJSON_CreateIntArray(stats.attrSkillUps, 8));
	cJSON_AddItemToObject(st, "attr_damage", floats(stats.attrDamage, 8));
	cJSON_AddItemToObject(st, "skill_damage", floats(stats.skillDamage, 27));
	cJSON_AddItemToObject(root, "last_outside", floats(lastOutside, 3));    // (jail / Intervention from inside)
	if (markCell >= 0)
	{
		cJSON* mark = cJSON_AddObjectToObject(root, "mark");
		cJSON_AddStringToObject(mark, "cell", cells[markCell].file.c_str());
		cJSON_AddItemToObject(mark, "pos", floats(markPos, 3));
		cJSON_AddNumberToObject(mark, "yaw", markYaw);
	}
	cJSON* books = cJSON_AddArrayToObject(st, "books_read");
	for (auto& b : stats.booksRead)
		cJSON_AddItemToArray(books, cJSON_CreateString(b.c_str()));
	cJSON* sp = cJSON_AddArrayToObject(st, "spells");
	for (auto& s : stats.spells)
		cJSON_AddItemToArray(sp, cJSON_CreateString(s.c_str()));
	cJSON_AddStringToObject(st, "selected_spell", stats.selectedSpell.c_str());
	cJSON_AddStringToObject(st, "head", stats.head.c_str());
	cJSON_AddStringToObject(st, "hair", stats.hair.c_str());
	cJSON* qk = cJSON_AddArrayToObject(st, "quick_keys");
	for (auto& q : stats.quickKeys)
		cJSON_AddItemToArray(qk, cJSON_CreateString(q.c_str()));
	cJSON* pu = cJSON_AddObjectToObject(st, "powers_used");
	for (auto& p : stats.powerUsedDay)
		cJSON_AddNumberToObject(pu, p.first.c_str(), p.second);
	cJSON_AddNumberToObject(root, "bounty", bounty);
	cJSON_AddNumberToObject(root, "arrest_declined", arrestDeclined);
	cJSON_AddBoolToObject(root, "no_teleport", teleportDisabled);
	cJSON* le = cJSON_AddObjectToObject(root, "lazy_enabled");
	for (auto& e : lazyEnabled)
		cJSON_AddBoolToObject(le, e.first.c_str(), e.second);
	cJSON* tl = cJSON_AddObjectToObject(root, "topic_log");
	for (auto& t : topicLog)
	{
		cJSON* a = cJSON_AddArrayToObject(tl, t.first.c_str());
		for (auto& l : t.second)
			cJSON_AddItemToArray(a, cJSON_CreateString(l.c_str()));
	}
	cJSON* ci = cJSON_AddArrayToObject(root, "cleared_infos");
	for (auto& c : clearedInfos)
		cJSON_AddItemToArray(ci, cJSON_CreateString(c.c_str()));
	cJSON* cr = cJSON_AddObjectToObject(root, "cell_respawn");
	for (auto& c : cellRespawnAt)
		cJSON_AddNumberToObject(cr, c.first.c_str(), c.second);
	cJSON* cl = cJSON_AddObjectToObject(root, "cell_levels");
	for (auto& c : cellLevel)
		cJSON_AddNumberToObject(cl, c.first.c_str(), c.second);
	cJSON* wx = cJSON_AddObjectToObject(root, "weather");
	cJSON_AddNumberToObject(wx, "now", weatherNow);
	cJSON_AddNumberToObject(wx, "next", weatherNext);
	cJSON_AddNumberToObject(wx, "blend", weatherBlend);
	cJSON_AddNumberToObject(wx, "queued", weatherQueued);
	cJSON_AddNumberToObject(wx, "timer", weatherTimer);
	cJSON_AddStringToObject(wx, "region", weatherRegion.c_str());
	cJSON* fw = cJSON_AddObjectToObject(wx, "forced");
	for (auto& f : forcedWeather)
		cJSON_AddNumberToObject(fw, f.first.c_str(), f.second);
	cJSON* rc = cJSON_AddObjectToObject(wx, "chances");
	for (auto& r : regionChances)
		cJSON_AddItemToObject(rc, r.first.c_str(), cJSON_CreateIntArray(r.second.data(), r.second.size()));
	// Items the player enchanted: their enchantment and what they were made from
	cJSON* madeIt = cJSON_AddArrayToObject(root, "made_items");
	for (auto& id : madeItems)
	{
		const Object* o = game.object(id);
		auto en = o ? game.spells.find(o->ench) : game.spells.end();
		if (!o || en == game.spells.end())
			continue;
		cJSON* p = cJSON_CreateObject();
		cJSON_AddStringToObject(p, "id", id.c_str());
		cJSON_AddStringToObject(p, "base", o->iconBase.c_str());
		cJSON_AddStringToObject(p, "name", o->name.c_str());
		cJSON_AddNumberToObject(p, "value", o->value);
		cJSON_AddStringToObject(p, "ench", o->ench.c_str());
		cJSON_AddNumberToObject(p, "type", en->second.type);
		cJSON_AddNumberToObject(p, "cost", en->second.cost);
		cJSON_AddNumberToObject(p, "charge", en->second.charge);
		cJSON* ef = cJSON_AddArrayToObject(p, "effects");
		for (auto& e : en->second.effects)
		{
			int v[7] = { e.effect, e.skill, e.attribute, e.min, e.max, e.duration, e.range };
			cJSON_AddItemToArray(ef, cJSON_CreateIntArray(v, 7));
		}
		cJSON_AddItemToArray(madeIt, p);
	}
	// Spells the player made
	cJSON* made = cJSON_AddArrayToObject(root, "made_spells");
	for (auto& id : madeSpells)
	{
		auto sp = game.spells.find(id);
		if (sp == game.spells.end())
			continue;
		cJSON* p = cJSON_CreateObject();
		cJSON_AddStringToObject(p, "id", id.c_str());
		cJSON_AddStringToObject(p, "name", sp->second.name.c_str());
		cJSON_AddNumberToObject(p, "cost", sp->second.cost);
		cJSON* ef = cJSON_AddArrayToObject(p, "effects");
		for (auto& e : sp->second.effects)
		{
			int v[7] = { e.effect, e.skill, e.attribute, e.min, e.max, e.duration, e.range };
			cJSON_AddItemToArray(ef, cJSON_CreateIntArray(v, 7));
		}
		cJSON_AddItemToArray(made, p);
	}
	// Potions the player made: their objects, so the inventory's ids find them again
	cJSON* potions = cJSON_AddArrayToObject(root, "potions");
	for (auto& id : brewed)
		if (const Object* o = game.object(id))
		{
			cJSON* p = cJSON_CreateObject();
			cJSON_AddStringToObject(p, "id", o->id.c_str());
			cJSON_AddStringToObject(p, "name", o->name.c_str());
			cJSON_AddNumberToObject(p, "value", o->value);
			cJSON_AddNumberToObject(p, "weight", o->weight);
			cJSON_AddStringToObject(p, "icon", o->icon.c_str());
			cJSON_AddNumberToObject(p, "icon_ix", o->iconIx);
			cJSON* ef = cJSON_AddArrayToObject(p, "effects");
			for (auto& e : o->effects)
			{
				int v[5] = { e.effect, e.skill, e.attribute, e.min, e.duration };
				cJSON_AddItemToArray(ef, cJSON_CreateIntArray(v, 5));
			}
			cJSON_AddItemToArray(potions, p);
		}
	cJSON* stolenOwners = cJSON_AddObjectToObject(root, "stolen_from");
	for (auto& s : stolenFrom)
	{
		cJSON* a = cJSON_AddArrayToObject(stolenOwners, s.first.c_str());
		for (auto& o : s.second)
			cJSON_AddItemToArray(a, cJSON_CreateString(o.c_str()));
	}
	cJSON* stolenItems = cJSON_AddObjectToObject(root, "stolen");
	for (auto& s : stolen)
		cJSON_AddNumberToObject(stolenItems, s.first.c_str(), s.second);
	cJSON_AddNumberToObject(root, "reputation", pcReputation);
	cJSON* fr = cJSON_AddObjectToObject(root, "faction_ranks");
	for (auto& f : pcRank)
		cJSON_AddNumberToObject(fr, f.first.c_str(), f.second);
	cJSON* fp = cJSON_AddObjectToObject(root, "faction_rep");
	for (auto& f : pcFacRep)
		cJSON_AddNumberToObject(fp, f.first.c_str(), f.second);
	cJSON_AddNumberToObject(root, "corprus_since", corprusSince);
	cJSON* mk = cJSON_AddArrayToObject(root, "map_known");
	for (auto& m : mapKnown)
		cJSON_AddItemToArray(mk, cJSON_CreateString(m.c_str()));
	cJSON* mc = cJSON_AddArrayToObject(root, "map_cells");
	for (int k : mapCells)
		cJSON_AddItemToArray(mc, cJSON_CreateNumber(k));
	cJSON* ex = cJSON_AddArrayToObject(root, "expelled");
	for (auto& f : pcExpelled)
		cJSON_AddItemToArray(ex, cJSON_CreateString(f.c_str()));
	cJSON* fre = cJSON_AddObjectToObject(root, "faction_reactions");
	for (auto& f : factionReactions)
		cJSON_AddNumberToObject(fre, f.first.c_str(), f.second);

	cJSON* inv = cJSON_AddArrayToObject(root, "inventory");
	for (auto& it : inventory)
	{
		cJSON* o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "id", it.id.c_str());
		cJSON_AddNumberToObject(o, "count", it.count);
		cJSON_AddNumberToObject(o, "equipped", it.equipped);
		if (it.condition >= 0)
			cJSON_AddNumberToObject(o, "condition", it.condition);
		if (!it.soul.empty())
			cJSON_AddStringToObject(o, "soul", it.soul.c_str());
		if (it.charge >= 0.0f)
			cJSON_AddNumberToObject(o, "charge", it.charge);
		cJSON_AddItemToArray(inv, o);
	}
	cJSON* jl = cJSON_AddArrayToObject(root, "journal");
	for (auto& j : journal)
	{
		cJSON* o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "quest", j.quest.c_str());
		cJSON_AddNumberToObject(o, "index", j.index);
		cJSON_AddStringToObject(o, "text", j.text.c_str());
		cJSON_AddItemToArray(jl, o);
	}
	cJSON* ji = cJSON_AddObjectToObject(root, "journal_index");
	for (auto& q : journalIndex)
		cJSON_AddNumberToObject(ji, q.first.c_str(), q.second);
	cJSON* qf = cJSON_AddArrayToObject(root, "quests_finished");
	for (auto& q : questFinished)
		cJSON_AddItemToArray(qf, cJSON_CreateString(q.c_str()));
	cJSON* topics = cJSON_AddArrayToObject(root, "topics");
	for (auto& t : knownTopics)
		cJSON_AddItemToArray(topics, cJSON_CreateString(t.c_str()));
	cJSON* gl = cJSON_AddObjectToObject(root, "globals");
	for (auto& g : globals)
		cJSON_AddNumberToObject(gl, g.first.c_str(), g.second);

	// Objects are named by cell file + index within the cell: cells are read on demand, so the
	// global index differs from session to session. Only changed ones; cells out of memory: as stored
	cJSON* rs = cJSON_AddArrayToObject(root, "refs");
	cJSON* sc = cJSON_AddArrayToObject(root, "scripts");
	for (auto& lc : cells)
		if (lc.refsLoaded)
			for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
				if (refHash(i) != refs[i].baseHash)
					cJSON_AddItemToArray(rs, refState(i));
	for (auto& sc0 : scripts)
		cJSON_AddItemToArray(sc, scriptState(sc0));
	for (auto& stored : cellStates)
	{
		cJSON* st = cJSON_Parse(stored.second.c_str());
		cJSON* a;
		while ((a = cJSON_DetachItemFromArray(cJSON_GetObjectItem(st, "refs"), 0)))
			cJSON_AddItemToArray(rs, a);
		while ((a = cJSON_DetachItemFromArray(cJSON_GetObjectItem(st, "scripts"), 0)))
			cJSON_AddItemToArray(sc, a);
		cJSON_Delete(st);
	}
	// Spell effects still running on the player (summons come back beside them on loading)
	cJSON* fx = cJSON_AddArrayToObject(root, "effects");
	for (auto& e : effects)
	{
		cJSON* o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "e", e.effect);
		cJSON_AddNumberToObject(o, "attr", e.attribute);
		cJSON_AddNumberToObject(o, "skill", e.skill);
		cJSON_AddNumberToObject(o, "mag", e.magnitude);
		cJSON_AddNumberToObject(o, "left", e.remaining);
		cJSON_AddStringToObject(o, "src", e.source.c_str());
		if (e.ref >= 0 && e.ref < (int)refs.size())
			cJSON_AddStringToObject(o, "summon", refs[e.ref].idLower.c_str());
		if (!e.item.empty())
			cJSON_AddStringToObject(o, "item", e.item.c_str());
		if (!e.prev.empty())
			cJSON_AddStringToObject(o, "prev", e.prev.c_str());
		cJSON_AddItemToArray(fx, o);
	}
	// Creatures placed by scripts (not summons): in memory, and those waiting for their cell
	cJSON* placed = cJSON_AddArrayToObject(root, "placed");
	auto addPlaced = [&](const std::string& id, const std::string& cell, const float* pos, float yaw, float health, bool dead) {
		cJSON* o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "id", id.c_str());
		cJSON_AddStringToObject(o, "c", cell.c_str());
		cJSON_AddItemToObject(o, "pos", floats(pos, 3));
		cJSON_AddNumberToObject(o, "yaw", yaw);
		cJSON_AddNumberToObject(o, "health", health);
		cJSON_AddNumberToObject(o, "dead", dead);
		cJSON_AddItemToArray(placed, o);
	};
	for (int i : spawned)
		if (refs[i].spawnedRef && !refs[i].ally && !refs[i].dropped)
			addPlaced(refs[i].idLower, cells[refs[i].cell].file, refs[i].pos, refs[i].rot[2], refs[i].health, refs[i].dead);
	for (auto& p : pendingSpawns)
		addPlaced(p.id, p.cell, p.pos, p.yaw, p.health, p.dead);
	// Dropped items: in memory, and those waiting for their cell
	cJSON* dr = cJSON_AddArrayToObject(root, "dropped");
	auto addDropped = [&](const InventoryItem& it, const std::string& cell, const float* pos, float yaw) {
		cJSON* o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "id", it.id.c_str());
		cJSON_AddNumberToObject(o, "n", it.count);
		if (it.condition >= 0)
			cJSON_AddNumberToObject(o, "cond", it.condition);
		if (!it.soul.empty())
			cJSON_AddStringToObject(o, "soul", it.soul.c_str());
		if (it.charge >= 0.0f)
			cJSON_AddNumberToObject(o, "charge", it.charge);
		cJSON_AddStringToObject(o, "c", cell.c_str());
		cJSON_AddItemToObject(o, "pos", floats(pos, 3));
		cJSON_AddNumberToObject(o, "yaw", yaw);
		cJSON_AddItemToArray(dr, o);
	};
	for (int i : spawned)
		if (refs[i].dropped && !refs[i].pickedUp && refs[i].cell >= 0)
			addDropped({ refs[i].idLower, refs[i].count, false, refs[i].dropState.condition, refs[i].dropState.soul,
				refs[i].dropState.charge }, cells[refs[i].cell].file, refs[i].pos, refs[i].rot[2]);
	for (auto& d : droppedWaiting)
		addDropped(d.item, d.cell, d.pos, d.yaw);
	cJSON* dc = cJSON_AddObjectToObject(root, "dead_counts");
	for (auto& d : deadCounts)
		cJSON_AddNumberToObject(dc, d.first.c_str(), d.second);

	char* text = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	// Write to a temporary file first so a failed write never destroys the previous save
	std::string tmp = std::string(path) + ".tmp";
	// Compressed: a save is mostly repetitive JSON (about a tenth of the size)
	uLong rawLen = strlen(text);
	uLongf packedLen = compressBound(rawLen);
	std::vector<u8> packed(8 + packedLen);
	bool ok = compress2(packed.data() + 8, &packedLen, (const Bytef*)text, rawLen, 6) == Z_OK;
	free(text);
	memcpy(packed.data(), "MWZ1", 4);
	u32 raw32 = rawLen;
	memcpy(packed.data() + 4, &raw32, 4);
	MarkScope mark("save write");
	FILE* f = ok ? fopen(tmp.c_str(), "wb") : nullptr;
	ok = f && fwrite(packed.data(), 1, 8 + packedLen, f) == 8 + packedLen;
	if (f)
		fclose(f);
	if (ok)
	{
		remove(path);
		ok = rename(tmp.c_str(), path) == 0;
	}
	logf("save: %s %s (%s)", path, ok ? "written" : "FAILED", cellName().c_str());
	if (ok)
		writeSaveMeta(path, stats.name, cellName(), stats.level, osGetTime());
	return ok;
}

static cJSON* readSave(const char* path)
{
	char* text = zreadAll(path);      // saves are zlib-compressed ('MWZ1'); older plain ones still load
	if (!text)
		return nullptr;
	cJSON* root = cJSON_Parse(text);
	free(text);
	if (root && num(root, "version") != kSaveVersion)
	{
		logf("save: %s has version %d, ignored", path, (int)num(root, "version"));
		cJSON_Delete(root);
		return nullptr;
	}
	return root;
}

std::string World::savedCell(const char* path)
{
	cJSON* root = readSave(path);
	std::string cell = root ? str(root, "cell") : "";
	cJSON_Delete(root);
	return cell;
}

bool World::applySave(const char* path)
{
	logf("save: applying %s", path);
	cJSON* root = readSave(path);
	if (!root)
		return false;
	const cJSON* it;
	readFloats(cJSON_GetObjectItem(root, "feet"), player.feet, 3);
	player.yaw = num(root, "yaw");
	player.pitch = num(root, "pitch");
	player.vz = 0.0f;
	player.fallTop = player.feet[2];            // a load isn't a fall (it began at 0: a drop to the floor of a deep cell)
	player.landedFall = 0.0f;
	time = num(root, "time");
	gameHour = num(root, "hour", 9.0);
	dayCounted = -1;                    // (the calendar globals come from the save: no days to move them for)
	hourWritten = -1.0f;
	controlsEnabled = num(root, "controls", 1) != 0;
	jumpingEnabled = num(root, "jumping", 1) != 0;
	// (a save without these is from before they were kept: fighting and magic on, as for controls)
	fightingEnabled = num(root, "fighting", 1) != 0;
	magicEnabled = num(root, "magic", 1) != 0;
	controlsOff = (unsigned)num(root, "controlsoff");
	moveFlags.clear();
	forceSneak = false;
	menusEnabled = (unsigned)num(root, "menus");

	const cJSON* st = cJSON_GetObjectItem(root, "stats");
	stats.name = str(st, "name");
	stats.race = str(st, "race");
	stats.cls = str(st, "class");
	stats.birthsign = str(st, "birthsign");
	stats.female = num(st, "female") != 0;
	stats.level = (int)num(st, "level", 1);
	stats.levelProgress = (int)num(st, "level_progress");
	stats.healthBonus = num(st, "health_bonus");
	readFloats(cJSON_GetObjectItem(st, "skill_progress"), stats.skillProgress, 27);
	auto readInts = [](const cJSON* a, int* out, int n) {
		for (int i = 0; i < n && i < cJSON_GetArraySize(a); i++)
			out[i] = cJSON_GetArrayItem(a, i)->valueint;
	};
	readInts(cJSON_GetObjectItem(st, "skill_bonus"), stats.skillBonus, 27);
	readInts(cJSON_GetObjectItem(st, "attr_bonus"), stats.attrBonus, 8);
	readInts(cJSON_GetObjectItem(st, "attr_ups"), stats.attrSkillUps, 8);
	memset(stats.attrDamage, 0, sizeof(stats.attrDamage));
	memset(stats.skillDamage, 0, sizeof(stats.skillDamage));
	readFloats(cJSON_GetObjectItem(st, "attr_damage"), stats.attrDamage, 8);
	readFloats(cJSON_GetObjectItem(st, "skill_damage"), stats.skillDamage, 27);
	memset(lastOutside, 0, sizeof(lastOutside));
	readFloats(cJSON_GetObjectItem(root, "last_outside"), lastOutside, 3);
	markCell = -1;
	if (const cJSON* mark = cJSON_GetObjectItem(root, "mark"))
	{
		markCell = cellIndex(str(mark, "cell"));
		readFloats(cJSON_GetObjectItem(mark, "pos"), markPos, 3);
		markYaw = num(mark, "yaw");
	}
	stats.booksRead.clear();
	const cJSON* book;
	cJSON_ArrayForEach(book, cJSON_GetObjectItem(st, "books_read"))
		if (cJSON_IsString(book))
			stats.booksRead.insert(book->valuestring);
	stats.spells.clear();
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(st, "spells"))
		if (cJSON_IsString(it))
			stats.spells.push_back(it->valuestring);
	stats.selectedSpell = str(st, "selected_spell");
	stats.head = str(st, "head");
	stats.hair = str(st, "hair");
	for (int k = 0; k < 4; k++)
	{
		const cJSON* q = cJSON_GetArrayItem(cJSON_GetObjectItem(st, "quick_keys"), k);
		stats.quickKeys[k] = cJSON_IsString(q) ? q->valuestring : "";
	}
	stats.powerUsedDay.clear();
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(st, "powers_used"))
		stats.powerUsedDay[it->string] = it->valueint;
	bounty = (int)num(root, "bounty");
	pcReputation = (int)num(root, "reputation");
	pcRank.clear();
	pcFacRep.clear();
	const cJSON* f;
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "faction_ranks"))
		pcRank[f->string] = f->valueint;
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "faction_rep"))
		pcFacRep[f->string] = f->valueint;
	corprusSince = num(root, "corprus_since", -1.0);
	mapKnown.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "map_known"))
		if (cJSON_IsString(f))
			mapKnown.insert(f->valuestring);
	mapCells.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "map_cells"))
		if (cJSON_IsNumber(f))
			mapCells.insert(f->valueint);
	// A save from before the cells were kept: the named places it knew
	if (!cJSON_GetObjectItem(root, "map_cells"))
		for (auto& c : cells)
			if (!c.interior && !c.name.empty() && mapKnown.count(lower(c.name)))
				mapCells.insert(mapCellKey(c.gx, c.gy));
	pcExpelled.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "expelled"))
		if (cJSON_IsString(f))
			pcExpelled.insert(f->valuestring);
	factionReactions.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "faction_reactions"))
		factionReactions[f->string] = f->valueint;
	arrestDeclined = (int)num(root, "arrest_declined", -1);
	teleportDisabled = cJSON_IsTrue(cJSON_GetObjectItem(root, "no_teleport"));
	lazyEnabled.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "lazy_enabled"))
		lazyEnabled[f->string] = cJSON_IsTrue(f);
	// ones whose cell was read already (the player's)
	for (auto it = lazyEnabled.begin(); it != lazyEnabled.end();)
	{
		int ri = findRef(it->first);
		if (ri < 0)
		{
			++it;
			continue;
		}
		setEnabled(ri, it->second);
		it = lazyEnabled.erase(it);
	}
	topicLog.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "topic_log"))
	{
		const cJSON* l;
		cJSON_ArrayForEach(l, f)
			if (cJSON_IsString(l))
				topicLog[f->string].push_back(l->valuestring);
	}
	clearedInfos.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "cleared_infos"))
		if (cJSON_IsString(f))
			clearedInfos.insert(f->valuestring);
	cellLevel.clear();
	cellRespawnAt.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "cell_respawn"))
		cellRespawnAt[f->string] = (float)f->valuedouble;
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "cell_levels"))
		cellLevel[f->string] = f->valueint;
	if (const cJSON* wx = cJSON_GetObjectItem(root, "weather"))
	{
		weatherNow = (int)num(wx, "now");
		weatherNext = (int)num(wx, "next");
		weatherBlend = (float)num(wx, "blend", 1.0);
		weatherQueued = (int)num(wx, "queued", -1);
		weatherTimer = (float)num(wx, "timer", game.weatherHours);
		weatherSeen = -1.0f;
		weatherCell = -1;
		if (weatherNext != weatherNow && weatherBlend >= 1.0f)
			weatherBlend = 0.0f;
		weatherRegion = str(wx, "region");
		forcedWeather.clear();
		regionChances.clear();
		cJSON_ArrayForEach(f, cJSON_GetObjectItem(wx, "forced"))
			forcedWeather[f->string] = f->valueint;
		cJSON_ArrayForEach(f, cJSON_GetObjectItem(wx, "chances"))
		{
			const cJSON* v;
			cJSON_ArrayForEach(v, f)
				regionChances[f->string].push_back(v->valueint);
		}
	}
	madeItems.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "made_items"))
	{
		const Object* base = game.object(str(f, "base"));
		if (!base)
			continue;
		SpellDef en;
		en.id = str(f, "ench");
		en.type = (int)num(f, "type");
		en.cost = (int)num(f, "cost");
		en.charge = (int)num(f, "charge");
		const cJSON* e;
		cJSON_ArrayForEach(e, cJSON_GetObjectItem(f, "effects"))
		{
			int v[7] = {};
			for (int k = 0; k < 7; k++)
				v[k] = cJSON_GetArrayItem(e, k) ? cJSON_GetArrayItem(e, k)->valueint : 0;
			en.effects.push_back({ v[0], v[1], v[2], v[3], v[4], v[5], v[6] });
		}
		Object o = *base;
		o.id = str(f, "id");
		o.name = str(f, "name");
		o.value = (int)num(f, "value");
		o.ench = en.id;
		o.magic = true;
		o.iconBase = base->id;
		o.script.clear();
		game.spells[en.id] = en;
		game.objects[lower(o.id)] = o;
		madeItems.push_back(lower(o.id));
	}
	madeSpells.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "made_spells"))
	{
		SpellDef sp;
		sp.id = str(f, "id");
		sp.name = str(f, "name");
		sp.type = 0;
		sp.cost = (int)num(f, "cost");
		const cJSON* e;
		cJSON_ArrayForEach(e, cJSON_GetObjectItem(f, "effects"))
		{
			int v[7] = {};
			for (int k = 0; k < 7; k++)
				v[k] = cJSON_GetArrayItem(e, k) ? cJSON_GetArrayItem(e, k)->valueint : 0;
			sp.effects.push_back({ v[0], v[1], v[2], v[3], v[4], v[5], v[6] });
		}
		game.spells[sp.id] = sp;
		madeSpells.push_back(sp.id);
	}
	brewed.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "potions"))
	{
		Object o;
		o.id = str(f, "id");
		o.type = "ALCH";
		o.name = str(f, "name");
		o.value = (int)num(f, "value");
		o.weight = (float)num(f, "weight");
		o.icon = str(f, "icon");
		o.iconIx = (int)num(f, "icon_ix", -1);
		const cJSON* e;
		cJSON_ArrayForEach(e, cJSON_GetObjectItem(f, "effects"))
		{
			SpellEffect se = {};
			se.effect = cJSON_GetArrayItem(e, 0)->valueint;
			se.skill = cJSON_GetArrayItem(e, 1)->valueint;
			se.attribute = cJSON_GetArrayItem(e, 2)->valueint;
			se.min = se.max = cJSON_GetArrayItem(e, 3)->valueint;
			se.duration = cJSON_GetArrayItem(e, 4)->valueint;
			o.effects.push_back(se);
		}
		game.objects[lower(o.id)] = o;
		brewed.push_back(lower(o.id));
	}
	stolenFrom.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "stolen_from"))
	{
		const cJSON* o;
		cJSON_ArrayForEach(o, f)
			if (cJSON_IsString(o))
				stolenFrom[f->string].push_back(o->valuestring);
	}
	stolen.clear();
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "stolen"))
		stolen[f->string] = f->valueint;
	// Running spell effects (before the stats, which they change); summons come back at the end
	effects.clear();
	std::vector<std::pair<size_t, std::string>> summons;
	cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "effects"))
	{
		ActiveEffect e = { (int)num(f, "e"), (int)num(f, "attr", -1), (int)num(f, "skill", -1),
			(float)num(f, "mag"), (float)num(f, "left"), str(f, "src") };
		e.item = str(f, "item");
		e.prev = str(f, "prev");
		if (!str(f, "summon").empty())
			summons.emplace_back(effects.size(), str(f, "summon"));
		effects.push_back(e);
	}
	recomputeStats();
	float bars[6] = { stats.health, stats.healthMax, stats.magicka, stats.magickaMax, stats.fatigue, stats.fatigueMax };
	readFloats(cJSON_GetObjectItem(st, "bars"), bars, 6);
	stats.health = bars[0];
	stats.magicka = bars[2];
	stats.fatigue = bars[4];

	inventory.clear();
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "inventory"))
		inventory.push_back({ str(it, "id"), (int)num(it, "count"), num(it, "equipped") != 0, (int)num(it, "condition", -1),
			str(it, "soul"), (float)num(it, "charge", -1.0) });
	journal.clear();
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "journal"))
		journal.push_back({ str(it, "quest"), (int)num(it, "index"), str(it, "text") });
	journalIndex.clear();
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "journal_index"))
		journalIndex[it->string] = it->valueint;
	questFinished.clear();
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "quests_finished"))
		if (cJSON_IsString(it))
			questFinished.insert(it->valuestring);
	knownTopics.clear();
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "topics"))
		if (cJSON_IsString(it))
			knownTopics.insert(it->valuestring);
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "globals"))
		globals[it->string] = (float)it->valuedouble;

	// Objects and their scripts, grouped by cell: applied now to cells in memory, to the rest when
	// their objects are read (so a save doesn't read every cell it mentions)
	cellStates.clear();
	std::unordered_map<std::string, cJSON*> byCell;
	auto group = [&](const std::string& file, const char* kind) {
		cJSON*& g = byCell[file];
		if (!g)
		{
			g = cJSON_CreateObject();
			cJSON_AddArrayToObject(g, "refs");
			cJSON_AddArrayToObject(g, "scripts");
		}
		return cJSON_GetObjectItem(g, kind);
	};
	deadCounts.clear();
	const cJSON* dcs = cJSON_GetObjectItem(root, "dead_counts");
	cJSON_ArrayForEach(it, dcs)
		deadCounts[it->string] = it->valueint;
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "refs"))
	{
		cJSON_AddItemToArray(group(str(it, "c"), "refs"), cJSON_Duplicate(it, true));
		if (!dcs && num(it, "dead") != 0)
			deadCounts[str(it, "id")]++;          // saves from before dead_counts
	}
	// Global scripts and carried items' replace the running ones; objects' go with their cell
	std::vector<ScriptInstance> kept;
	for (auto& s : scripts)
		if (s.ref >= 0 && s.item.empty())
			kept.push_back(std::move(s));
	scripts = std::move(kept);
	rebuildScriptIndex();
	int skipped = 0;
	std::vector<std::pair<size_t, std::pair<std::string, int>>> targets;     // global scripts' references, found below
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "scripts"))
	{
		std::string item = str(it, "item");
		if (num(it, "ref", -1) >= 0 && item.empty())
		{
			cJSON_AddItemToArray(group(str(it, "ref_c"), "scripts"), cJSON_Duplicate(it, true));
			continue;
		}
		const Script* s = game.script(lower(str(it, "name")));
		if (!s)
		{
			skipped++;
			continue;
		}
		ScriptInstance inst;
		inst.script = s;
		inst.item = item;
		inst.running = num(it, "running", 1) != 0;
		inst.locals.assign(s->localNames.size(), 0.0f);
		readFloats(cJSON_GetObjectItem(it, "locals"), inst.locals.data(), inst.locals.size());
		if (item.empty() && cJSON_GetObjectItem(it, "tgt_c"))
			targets.emplace_back(scripts.size(), std::make_pair(str(it, "tgt_c"), (int)num(it, "tgt_i")));
		scripts.push_back(std::move(inst));
	}
	for (auto& g : byCell)
	{
		char* t = cJSON_PrintUnformatted(g.second);
		cellStates[g.first] = t;
		free(t);
		cJSON_Delete(g.second);
	}
	for (size_t c = 0; c < cells.size(); c++)
		if (cells[c].refsLoaded)
			applyCellState(c);
	// Placed creatures: at once where their cell's objects are in memory, else when it is read
	pendingSpawns.clear();
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "placed"))
	{
		PendingSpawn p = { str(it, "id"), str(it, "c"), {}, (float)num(it, "yaw"), (float)num(it, "health"),
			num(it, "dead") != 0 };
		readFloats(cJSON_GetObjectItem(it, "pos"), p.pos, 3);
		int c = cellIndex(p.cell);
		if (c >= 0 && cells[c].refsLoaded)
		{
			int ri = spawnActor(p.id, p.pos, p.yaw, c);
			if (ri >= 0)
			{
				refs[ri].health = p.health;
				refs[ri].dead = p.dead;
			}
		}
		else
			pendingSpawns.push_back(p);
	}
	droppedWaiting.clear();
	cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "dropped"))
	{
		DroppedItem d;
		d.item = { str(it, "id"), (int)num(it, "n", 1), false, (int)num(it, "cond", -1), str(it, "soul"),
			(float)num(it, "charge", -1.0) };
		d.cell = str(it, "c");
		d.yaw = (float)num(it, "yaw");
		readFloats(cJSON_GetObjectItem(it, "pos"), d.pos, 3);
		int c = cellIndex(d.cell);
		if (c >= 0 && cells[c].refsLoaded)
			dropItem(d.item, d.pos, d.yaw, c);
		else
			droppedWaiting.push_back(d);
	}
	// Cells with references moved elsewhere are read now, so those stand where they were moved to
	for (auto& g : byCell)
		if (strstr(cellStates.count(g.first) ? cellStates[g.first].c_str() : "", "\"at\":"))
			ensureRefs(cellIndex(g.first));
	// Global scripts started on an object: it is read if need be
	for (auto& t : targets)
	{
		int c = cellIndex(t.second.first);
		if (c >= 0 && ensureRefs(c) && t.second.second >= 0 && t.second.second < cells[c].refCount)
			scripts[t.first].target = cells[c].refBase + t.second.second;
	}
	cJSON_Delete(root);

	streamExterior(true);          // outdoors: the cells around the saved position
	refreshHidden();
	// Summoned creatures of running spells appear beside the player again
	for (size_t k = 0; k < summons.size(); k++)
	{
		ActiveEffect& e = effects[summons[k].first];
		float a = player.yaw + 0.6f * (k % 2 ? 1.0f : -1.0f);
		float pos[3] = { player.feet[0] + sinf(a) * 128.0f, player.feet[1] + cosf(a) * 128.0f, player.feet[2] };
		e.ref = spawnActor(summons[k].second, pos, player.yaw);
		if (e.ref < 0)
		{
			e.remaining = 0.0f;       // ends at once
			continue;
		}
		refs[e.ref].ally = true;
		refs[e.ref].aiPackage = AIPKG_FOLLOW;
		refs[e.ref].aiTarget = "player";
	}
	refreshStats();                // worn constant enchantments (the inventory came after the stats)
	for (size_t i = 0; i < refs.size(); i++)
		if (active(i))
			syncActor(i);
	logf("save: loaded %s, %s at %.0f %.0f %.0f%s", path, cellName().c_str(), player.feet[0], player.feet[1],
		player.feet[2], skipped ? " (some entries no longer match the data)" : "");
	return true;
}
