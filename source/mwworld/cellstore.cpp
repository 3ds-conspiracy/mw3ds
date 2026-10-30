// A cell's references: read from its file, their contents and actors, respawn, and its saved state
#include "mwworld/worldimp.hpp"
#include "components/files/datapath.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "extern/cjson/cJSON.h"
extern "C" u32 __ctru_heap_size;
#include <malloc.h>
#include "components/debug/debuglog.hpp"
#include "components/files/zfile.hpp"
#include "mwworld/worldhelpers.hpp"
#include <vector>
#include <zlib.h>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwstate/statehelpers.hpp"

static bool isItemType(const Atom& t)
{
	for (auto* it : kItemTypes)
		if (t == it)
			return true;
	return false;
}

static char* readText(const char* path)
{
	return zreadAll(path);
}

static void readVec3(const cJSON* a, float* out)
{
	for (int i = 0; i < 3; i++)
		out[i] = (float)cJSON_GetArrayItem(a, i)->valuedouble;
}

// An actor reference's starting state from its record: stats, AI package, arrows
// What a container / NPC / creature holds: its record's items, leveled lists picked for the player's
// level (seeded, so the same each time the cell is read; "each": every one of the count separately)
// An interior's water at a new height: its surface batches move with it (the converter gives a cell whose scripts move
// the water a surface over its whole floor plan: the floors hide what lies below them)
void moveCellWater(Cell& c, float z)
{
	float d = z - c.waterZ;
	c.waterZ = z;
	if (d == 0.0f)
		return;
	for (auto& b : c.batches)
		if (b.flags & BATCH_SCROLL)
		{
			b.posOffset[2] += d;
			b.bmin[2] += d;
			b.bmax[2] += d;
		}
}

// GetWaterLevel (OpenMW): outdoors 0 (the original game's oddity, the sea is at -1), an interior's water height, or the
// lowest number there is for one with no water
float World::scriptWaterLevel() const
{
	if (current < 0 || !cells[current].interior)
		return 0.0f;
	const LoadedCell* l = cells[current].live;
	auto it = waterLevels.find(lower(cells[current].name.str()));
	if (l && l->cell.hasWater())
		return l->cell.waterZ;
	if (it != waterLevels.end())
		return it->second;
	return -3.4e38f;
}

// SetWaterLevel / ModWaterLevel: the player's interior only (OpenMW refuses an exterior); kept for the cell
void World::setScriptWaterLevel(float z)
{
	if (current < 0 || !cells[current].interior)
	{
		logf("script: can't set the water level outdoors");
		return;
	}
	waterLevels[lower(cells[current].name.str())] = z;
	if (LoadedCell* l = cells[current].live)
		moveCellWater(l->cell, z);
}

void fillContents(const GameData& g, Ref& ref, int level, unsigned seed)
{
	if (!ref.obj)
		return;
	for (auto& item : ref.obj->items)
	{
		// (a negative count is a restocking quantity: its sign stays with the stack)
		int count = item.first;
		std::string id = lower(item.second);
		if (count == 0)
			continue;
		auto lv = g.leveled.find(id);
		if (lv == g.leveled.end())
		{
			if (g.object(id))
				ref.contents.emplace_back(count, id);
			continue;
		}
		// "each" rolls once per unit of a top-level line with more than one
		int picks = levRolls(lv->second.each, count);
		for (int k = 0; k < picks; k++)
		{
			std::string pick = g.pickLeveled(id, level, &seed);
			if (pick.empty() || !g.object(pick))
				continue;
			int n = lv->second.each ? (count < 0 ? -1 : 1) : count;
			bool stacked = false;
			for (auto& c : ref.contents)
				if (c.second == pick)
				{
					// (OpenMW's addItems: the sum of the sizes, negative when either was)
					c.first = (c.first < 0 || n < 0 ? -1 : 1) * (stockCount(c.first) + stockCount(n));
					stacked = true;
					break;
				}
			if (!stacked)
				ref.contents.emplace_back(n, pick);
		}
	}
}

void initActorState(const GameData& g, Ref& ref)
{
	if (ref.actor < 0 || ref.actor >= (int)g.actors.size())
		return;
	const ActorDef& a = g.actors[ref.actor];
	ref.disposition = a.disposition;
	// A record with no health is a corpse placed in the world (dead_ NPCs, dead_skeleton ...): it lies in its death pose
	ref.health = a.health;
	ref.healthMax = std::max(a.health, 1);
	if (a.health <= 0)
		ref.dead = true;
	ref.fatigue = ref.fatigueMax = a.fatigue;
	ref.magicka = ref.magickaMax = a.magicka;
	ref.gold = a.gold;
	ref.aiPackage = a.aiPackage;
	ref.aiTarget = a.aiTarget;
	memcpy(ref.aiDest, a.aiDest, sizeof(ref.aiDest));
	ref.aiDuration = a.aiDuration;
	// Ammunition they carry for their bow (or thrown weapons, their own ammunition)
	const std::string& ammo = !a.ammo.empty() ? a.ammo : a.weapon;
	for (auto& c : ref.contents)
		if (c.second == ammo && g.object(ammo) && g.object(ammo)->subtype >= WEAP_THROWN)
			ref.ammo += stockCount(c.first);
}

bool parseRefs(const World& w, int cellIndex, ParsedRefs& out)
{
	const LevelCell& lc = w.cells[cellIndex];
	std::string p = cellPath(w.dataDir, lc.file, ".json");
	const char* path = p.c_str();
	char* text = readText(path);
	cJSON* root = text ? cJSON_Parse(text) : nullptr;
	free(text);
	if (!root)
	{
		logf("world: cannot read %s", path);
		return false;
	}
	const cJSON* r;
	cJSON_ArrayForEach(r, cJSON_GetObjectItem(root, "refs"))
	{
		Ref ref;
		ref.cell = cellIndex;
		ref.id = cJSON_GetObjectItem(r, "id")->valuestring;
		ref.idLower = lower(ref.id);
		ref.type = cJSON_GetObjectItem(r, "type")->valuestring; ref.recType = recTypeOf(ref.type.c_str());
		readVec3(cJSON_GetObjectItem(r, "pos"), ref.pos);
		readVec3(cJSON_GetObjectItem(r, "rot"), ref.rot);
		memcpy(ref.home, ref.pos, sizeof(ref.home));
		ref.scale = (float)cJSON_GetObjectItem(r, "scale")->valuedouble;
		const cJSON* v;
		if ((v = cJSON_GetObjectItem(r, "count"))) ref.count = v->valueint;
		if ((v = cJSON_GetObjectItem(r, "lock"))) ref.lockLevel = v->valueint;
		if ((v = cJSON_GetObjectItem(r, "key"))) ref.key = v->valuestring;
		if ((v = cJSON_GetObjectItem(r, "owner"))) ref.owner = v->valuestring;
		if ((v = cJSON_GetObjectItem(r, "ofac"))) ref.ownerFaction = v->valuestring;
		if ((v = cJSON_GetObjectItem(r, "orank"))) ref.ownerRank = v->valueint;
		if ((v = cJSON_GetObjectItem(r, "oglob"))) ref.ownerGlobal = v->valuestring;
		if ((v = cJSON_GetObjectItem(r, "trap"))) ref.trap = v->valuestring;
		if ((v = cJSON_GetObjectItem(r, "lev"))) ref.levList = v->valuestring;
		if ((v = cJSON_GetObjectItem(r, "actor"))) ref.actor = v->valueint;
		if ((v = cJSON_GetObjectItem(r, "door_mesh"))) ref.doorMesh = v->valueint;
		if ((v = cJSON_GetObjectItem(r, "dest")))
		{
			ref.hasDest = true;
			const cJSON* c = cJSON_GetObjectItem(v, "cell");
			ref.destCell = cJSON_IsString(c) ? c->valuestring : lc.name;
			readVec3(cJSON_GetObjectItem(v, "pos"), ref.destPos);
			readVec3(cJSON_GetObjectItem(v, "rot"), ref.destRot);
			ref.destEnd = cJSON_IsTrue(cJSON_GetObjectItem(v, "end"));
			ref.destUnconverted = cJSON_IsTrue(cJSON_GetObjectItem(v, "unconverted"));
			if (const cJSON* g = cJSON_GetObjectItem(v, "grid"))
			{
				ref.destHasGrid = true;
				ref.destGrid[0] = cJSON_GetArrayItem(g, 0)->valueint;
				ref.destGrid[1] = cJSON_GetArrayItem(g, 1)->valueint;
			}
		}
		if ((v = cJSON_GetObjectItem(r, "bbox")))
		{
			ref.hasBox = true;
			readVec3(cJSON_GetArrayItem(v, 0), ref.boxMin);
			readVec3(cJSON_GetArrayItem(v, 1), ref.boxMax);
		}
		// A soul gem with a soul in it, an enchanted item with some charge (placed so in the world)
		if (const cJSON* soul = cJSON_GetObjectItem(r, "soul"))
			if (cJSON_IsString(soul))
				ref.dropState.soul = soul->valuestring;
		if (const cJSON* ch = cJSON_GetObjectItem(r, "charge"))
			ref.dropState.charge = (float)ch->valuedouble;
		if (const cJSON* col = cJSON_GetObjectItem(r, "col"))
		{
			ref.colFirst = cJSON_GetArrayItem(col, 0)->valueint;
			ref.colCount = cJSON_GetArrayItem(col, 1)->valueint;
		}
		const cJSON* rg;
		cJSON_ArrayForEach(rg, cJSON_GetObjectItem(r, "ranges"))
			ref.ranges.push_back({ cJSON_GetArrayItem(rg, 0)->valueint, (u32)cJSON_GetArrayItem(rg, 1)->valueint,
				(u32)cJSON_GetArrayItem(rg, 2)->valueint });
		ref.obj = w.game.object(ref.id);
		if (ref.recType == REC_CONT || ref.recType == REC_NPC_ || ref.recType == REC_CREA)
		{
			unsigned seed = 2166136261u;
			for (char ch : lc.file.view())
				seed = (seed ^ (u8)ch) * 16777619u;
			fillContents(w.game, ref, w.stats.level, seed ^ (unsigned)out.refs.size() * 2654435761u);
		}
		initActorState(w.game, ref);
		memcpy(ref.start, ref.pos, sizeof(ref.pos));
		ref.start[3] = ref.rot[2];
		ref.startRot[0] = ref.rot[0];
		ref.startRot[1] = ref.rot[1];
		memcpy(ref.boxStart, ref.boxMin, sizeof(ref.boxMin));
		memcpy(ref.boxStart + 3, ref.boxMax, sizeof(ref.boxMax));
		out.refs.push_back(std::move(ref));
	}
	// Path grid: points in world space and the points each connects to
	if (const cJSON* pg = cJSON_GetObjectItem(root, "pathgrid"))
	{
		const cJSON* p;
		cJSON_ArrayForEach(p, cJSON_GetObjectItem(pg, "points"))
		{
			PathPoint pp;
			readVec3(p, pp.pos);
			pp.cell = cellIndex;
			out.points.push_back(pp);
		}
		const cJSON* e;
		cJSON_ArrayForEach(e, cJSON_GetObjectItem(pg, "edges"))
			out.edges.emplace_back(cJSON_GetArrayItem(e, 0)->valueint, cJSON_GetArrayItem(e, 1)->valueint);
	}
	cJSON_Delete(root);
	return true;
}

// A run of n free slots from the ranges evicted cells left, or -1: the smallest run that fits (the lowest of equals),
// so long runs stay whole for whole cells and the end of the list stays free to give back
int takeSlots(std::vector<std::pair<int, int>>& free, int n)
{
	int best = -1;
	for (size_t k = 0; k < free.size(); k++)
		if (free[k].second >= n && (best < 0 || free[k].second < free[best].second))
			best = (int)k;
	if (best < 0)
		return -1;
	int first = free[best].first;
	free[best].first += n;
	free[best].second -= n;
	if (free[best].second == 0)
		free.erase(free.begin() + best);
	return first;
}

// Slots given back: kept in order and joined to the runs next to them. Without the joining, the runs only ever split
// (a spawned creature takes one slot out of a cell's run), a cell read again rarely found one long enough, and
// World::refs grew for good: 44,000 slots of 832 bytes for 6,000 objects over the main quest, an Old 3DS's heap
void freeSlots(std::vector<std::pair<int, int>>& free, int first, int n)
{
	if (n <= 0)
		return;
	auto it = std::lower_bound(free.begin(), free.end(), std::make_pair(first, 0));
	it = free.insert(it, { first, n });
	if (it + 1 != free.end() && it->first + it->second == (it + 1)->first)
	{
		it->second += (it + 1)->second;
		free.erase(it + 1);
	}
	if (it != free.begin() && (it - 1)->first + (it - 1)->second == it->first)
	{
		(it - 1)->second += it->second;
		free.erase(it);
	}
}

// Adds a cell's parsed references to the world (in slots other cells left, where they fit), starts
// their scripts and puts back what changed in them before (cellStates)
void appendRefs(World& w, int cellIndex, ParsedRefs& p)
{
	LevelCell& lc = w.cells[cellIndex];
	if (lc.refsLoaded)
		return;
	lc.refsLoaded = true;
	lc.lastUsed = ++w.useTick;
	w.refsChanged();
	int n = p.refs.size();
	int base = n ? takeSlots(w.freeRefs, n) : -1;
	if (base < 0)
	{
		base = w.refs.size();
		w.refs.resize(base + n);
	}
	for (int k = 0; k < n; k++)
		w.refs[base + k] = std::move(p.refs[k]);
	lc.refBase = base;
	lc.refCount = n;
	int np = p.points.size();
	int pbase = np ? takeSlots(w.freePaths, np) : -1;
	if (pbase < 0)
	{
		pbase = w.pathPoints.size();
		w.pathPoints.resize(pbase + np);
	}
	for (int k = 0; k < np; k++)
		w.pathPoints[pbase + k] = p.points[k];
	lc.pathBase = pbase;
	for (auto& e : p.edges)
		if (e.first < np && e.second < np)
		{
			w.pathPoints[pbase + e.first].links.push_back(pbase + e.second);
			w.pathPoints[pbase + e.second].links.push_back(pbase + e.first);
		}
	lc.pathCount = np;
	// Outdoors each cell's grid stops at its edge (Morrowind never joins them): link points near the
	// border to close ones in the neighbouring cells already in memory, so routes cross town
	if (!lc.interior && np)
		for (size_t c = 0; c < w.cells.size(); c++)
		{
			const LevelCell& oc = w.cells[c];
			if (&oc == &lc || oc.interior || !oc.pathCount || !oc.refsLoaded || abs(oc.gx - lc.gx) > 1 || abs(oc.gy - lc.gy) > 1)
				continue;
			for (int a = pbase; a < pbase + np; a++)
				for (int b = oc.pathBase; b < oc.pathBase + oc.pathCount; b++)
				{
					PathPoint& pa = w.pathPoints[a];
					PathPoint& pb = w.pathPoints[b];
					float dx = pa.pos[0] - pb.pos[0], dy = pa.pos[1] - pb.pos[1], dz = pa.pos[2] - pb.pos[2];
					if (dx * dx + dy * dy > 350.0f * 350.0f || fabsf(dz) > 120.0f)
						continue;
					pa.links.push_back(b);
					pb.links.push_back(a);
				}
		}
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
		w.startScriptFor(i);
	if (w.postChargen)
		for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
			w.postChargenRef(w.refs[i]);
	// Leveled creatures: each spot's pick for the player's level when the cell was first seen (the same
	// pick every time the cell is read again); nothing there when the list says so
	auto lvl = w.cellLevel.find(lc.file);
	int level = lvl != w.cellLevel.end() ? lvl->second : (w.cellLevel[lc.file] = w.stats.level);
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
	{
		Ref& r = w.refs[i];
		if (r.levList.empty())
			continue;
		unsigned seed = 2166136261u;
		for (char ch : lc.file.view())
			seed = (seed ^ (u8)ch) * 16777619u;
		seed ^= (unsigned)(i - lc.refBase) * 2654435761u;
		std::string pick = w.game.pickLeveled(r.levList, level, &seed);
		if (pick.empty())
			r.enabled = false;
		else
			w.applyLevPick(i, pick);
		logf("world: %s: %s (level %d) -> %s", lc.file.c_str(), r.levList.c_str(), level, pick.empty() ? "nothing" : pick.c_str());
	}
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
		w.refs[i].baseHash = w.refHash(i);
	w.applyCellState(cellIndex);
	if (!w.lazyEnabled.empty())
		for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
		{
			auto le = w.lazyEnabled.find(w.refs[i].idLower);
			if (le == w.lazyEnabled.end())
				continue;
			w.setEnabled(i, le->second);
			w.lazyEnabled.erase(le);
		}
	for (size_t k = 0; k < w.droppedWaiting.size();)
	{
		if (w.droppedWaiting[k].cell != lc.file)
		{
			k++;
			continue;
		}
		World::DroppedItem d = w.droppedWaiting[k];
		w.droppedWaiting.erase(w.droppedWaiting.begin() + k);
		w.dropItem(d.item, d.pos, d.yaw, cellIndex);
	}
	for (size_t k = 0; k < w.pendingSpawns.size();)
	{
		World::PendingSpawn p = w.pendingSpawns[k];
		if (p.cell != lc.file)
		{
			k++;
			continue;
		}
		w.pendingSpawns.erase(w.pendingSpawns.begin() + k);
		int ri = w.placeObject(p.id, cellIndex, p.pos, p.yaw);      // (a person or creature, or an object a script placed)
		if (ri >= 0 && w.refs[ri].actor >= 0)
		{
			w.refs[ri].health = p.health;
			w.refs[ri].dead = p.dead;
		}
	}
}

// OpenMW's CellStore::respawn, run as a cell loads. Containers with the Respawn flag refill when more than
// iMonthsToRespawn months have passed since the cell's last refresh (its stamp starts at day 1, hour 0: 24 hours
// before our clock). Actors are looked at every time: a corpse is cleared fCorpseClearDelay hours after death (a
// persistent one stays), and one with the Respawn flag gets up again.
void World::respawnCell(int c)
{
	LevelCell& lc = cells[c];
	if (!lc.refsLoaded)
		return;
	auto at = cellRespawnAt.find(lc.file);
	float last = at == cellRespawnAt.end() ? -24.0f : at->second;
	bool refill = gameHour - last > 24.0f * 30.0f * game.gmstf("imonthstorespawn", 4.0f);
	if (refill)
		cellRespawnAt[lc.file] = gameHour;
	float clearDelay = game.gmstf("fcorpsecleardelay", 72.0f);
	float respawnDelay = std::min(game.gmstf("fcorpserespawndelay", 72.0f), clearDelay);
	int restocked = 0, revived = 0, cleared = 0;
	for (int i = 0; i < (int)refs.size(); i++)
	{
		Ref& r = refs[i];
		if (r.cell != c && r.at != c)
			continue;
		if (refill && r.cell == c && r.recType == REC_CONT && r.obj && r.obj->respawn)
		{
			r.contents.clear();
			fillContents(game, r, stats.level, (unsigned)rand());
			restocked++;
		}
		else if (r.actor >= 0 && r.dead && r.diedAt >= 0.0f)
		{
			const ActorDef& def = game.actors[r.actor];
			if (def.respawn && gameHour - r.diedAt >= respawnDelay)
			{
				r.dead = r.died = r.murdered = false;
				r.health = r.healthMax;
				r.fatigue = r.fatigueMax;
				r.ai = AI_IDLE;
				r.diedAt = -1.0f;
				memcpy(r.pos, r.home, sizeof(r.pos));
				r.fitBox();
				r.contents.clear();
				fillContents(game, r, stats.level, (unsigned)rand());
				revived++;
			}
			// (the record's persistent flag isn't in our data: the essential and the scripted ones stand for it)
			else if (!def.essential && def.script.empty() && !r.pickedUp && gameHour - r.diedAt >= clearDelay)
			{
				deleteRef(i);
				cleared++;
			}
		}
	}
	if (restocked || revived || cleared)
		logf("world: %s refreshed: %d containers restocked, %d came back, %d corpses cleared", lc.file.c_str(), restocked, revived, cleared);
}

// The cells named by a cell's changes as where its people stand now ("at")
static void movedTargets(const std::string& json, std::vector<std::string>& to)
{
	for (size_t k = json.find("\"at\":\""); k != std::string::npos; k = json.find("\"at\":\"", k + 1))
	{
		size_t a = k + 6, e = json.find('"', a);
		if (e == std::string::npos)
			break;
		std::string t = json.substr(a, e - a);
		if (std::find(to.begin(), to.end(), t) == to.end())
			to.push_back(t);
	}
}

void World::storeCellState(const std::string& file, const std::string& json)
{
	StoredCell st;
	st.raw = json.size();
	st.movedRefs = json.find("\"at\":") != std::string::npos;
	movedTargets(json, st.movedTo);
	st.movedScanned = true;
	// (no memory to pack it: kept as text, raw 0 marks it so)
	if (!packText(json, st.z))
	{
		st.z.assign(json.begin(), json.end());
		st.raw = 0;
	}
	cellStates[file] = std::move(st);
}

// A cell's people a quest moved into this one (PositionCell) come with their own cell's objects: those are read back now
// so they stand here as its objects come in (applyCellState lists them). Mehra Milo, moved to Holamayan, belongs to
// the Library of Vivec: the library stayed in memory for the rest of the game to keep her; now it is read for the
// monastery and goes again with the rest
void World::readMovedIn(int cell)
{
	std::vector<std::string> homes;
	for (auto& st : cellStates)
	{
		// (a save's kept block says only that someone was moved: looked through once for where to)
		if (st.second.movedRefs && !st.second.movedScanned)
		{
			st.second.movedScanned = true;
			movedTargets(cellStateJson(st.second), st.second.movedTo);
		}
		if (std::find(st.second.movedTo.begin(), st.second.movedTo.end(), cells[cell].file) != st.second.movedTo.end())
			homes.push_back(st.first);
	}
	for (const std::string& h : homes)
	{
		int c = cellIndex(h);
		if (c >= 0 && !cells[c].refsLoaded && ensureRefs(c))
			logf("world: %s read for %s standing in %s", h.c_str(), "its people", cells[cell].file.c_str());
	}
}

std::string World::cellStateJson(const StoredCell& st) const
{
	if (st.raw == 0)
		return std::string(st.z.begin(), st.z.end());
	std::string json;
	if (!unpackText(st.z.data(), st.z.size(), st.raw, json))
	{
		logf("save: a kept cell's changes won't unpack (%u bytes packed)", (unsigned)st.z.size());
		return "";
	}
	return json;
}

// A cell's changed objects and its objects' scripts that hold anything, as text ("" when nothing)
std::string World::cellStateText(int c, bool* failed) const
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
		// (character creation's scripts: their locals start as postChargenRef leaves them, "done", so a zero here is a
		// change to keep too: reading the cell's objects again would mark the script done over it)
		set |= refs[s.ref].idLower.compare(0, 7, "chargen") == 0;
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
		if (t)
			out = t;
		else if (failed)
			*failed = true;
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
	std::string json = cellStateJson(found->second);
	cellStates.erase(found);
	cJSON* st = cJSON_Parse(json.c_str());
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
	bool movedIn = false;
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
				movedIn |= cells[ac].live != nullptr;
			}
		}
	}
	// (one moved into a cell already loaded joins its lists: the crosshair, the actors' updates. A save loaded in the
	// Temple Gardens read the Temple basement after the Gardens, and Shunari Eye-Fly, moved there by PositionCell, could
	// not be aimed at nor talked to until the next cell change)
	if (movedIn)
		rebuildLoadedLists();
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
