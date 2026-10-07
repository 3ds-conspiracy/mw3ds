#include "world.h"
#include "distant.h"
#include "datapath.h"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "log.h"
#include "zfile.h"

static const char* kItemTypes[] = { "MISC", "BOOK", "ALCH", "INGR", "WEAP", "ARMO", "CLOT", "LIGH", "APPA", "LOCK", "PROB", "REPA" };

static bool isItemType(const std::string& t)
{
	for (auto* it : kItemTypes)
		if (t == it)
			return true;
	return false;
}

float* ScriptInstance::local(const std::string& lowerName)
{
	int i = script ? script->localIndex(lowerName) : -1;
	return i < 0 ? nullptr : &locals[i];
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

static std::string cellFileName(const std::string& cellName)
{
	std::string out;
	for (char c : lower(cellName))
		out += ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') ? c : '_';
	return out;
}

static void hideRanges(World& w, Ref& ref);
static void showRanges(World& w, Ref& ref);

// An actor reference's starting state from its record: stats, AI package, arrows
// What a container / NPC / creature holds: its record's items, leveled lists picked for the player's
// level (seeded, so the same each time the cell is read; "each": every one of the count separately)
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

static void initActorState(const GameData& g, Ref& ref)
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

// Reads cells/<file>.json and appends its references
// A cell's references and path grid as read from cells/<file>.json. Reading only looks at the game
// data, so the streaming thread can do it; appendRefs (main thread) makes them part of the world.
struct ParsedRefs
{
	std::vector<Ref> refs;
	std::vector<PathPoint> points;
	std::vector<std::pair<int, int>> edges;
};

static bool parseRefs(const World& w, int cellIndex, ParsedRefs& out)
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
		ref.type = cJSON_GetObjectItem(r, "type")->valuestring;
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
		if (ref.type == "CONT" || ref.type == "NPC_" || ref.type == "CREA")
		{
			unsigned seed = 2166136261u;
			for (char ch : lc.file)
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

// A run of n free slots from the ranges evicted cells left, or -1
static int takeSlots(std::vector<std::pair<int, int>>& free, int n)
{
	for (size_t k = 0; k < free.size(); k++)
		if (free[k].second >= n)
		{
			int first = free[k].first;
			free[k].first += n;
			free[k].second -= n;
			if (free[k].second == 0)
				free.erase(free.begin() + k);
			return first;
		}
	return -1;
}

// Adds a cell's parsed references to the world (in slots other cells left, where they fit), starts
// their scripts and puts back what changed in them before (cellStates)
static void appendRefs(World& w, int cellIndex, ParsedRefs& p)
{
	LevelCell& lc = w.cells[cellIndex];
	if (lc.refsLoaded)
		return;
	lc.refsLoaded = true;
	lc.lastUsed = ++w.useTick;
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
		for (char ch : lc.file)
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
		int ri = w.spawnActor(p.id, p.pos, p.yaw, cellIndex);
		if (ri >= 0)
		{
			w.refs[ri].health = p.health;
			w.refs[ri].dead = p.dead;
		}
	}
}

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
	for (const std::vector<std::string>* list : { &a.spells, &a.combatSpells, &a.diseases })
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
		for (const std::vector<std::string>* list : { &game.actors[r.actor].spells, &game.actors[r.actor].diseases })
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

int World::spawnActor(const std::string& id, const float pos[3], float yaw, int inCell)
{
	auto sp = game.spawns.find(lower(id));
	if (sp == game.spawns.end() || sp->second.actor < 0 || sp->second.actor >= (int)game.actors.size())
	{
		logf("world: can't place %s (not converted for placing)", id.c_str());
		return -1;
	}
	const SpawnDef& d = sp->second;
	// The cell under the spot (outdoors a neighbour of the player's may hold it)
	int c = inCell >= 0 ? inCell : current;
	if (inCell < 0 && !cells[current].interior)
	{
		int g = gridCell((int)floorf(pos[0] / 8192.0f), (int)floorf(pos[1] / 8192.0f));
		if (g >= 0 && cells[g].live)
			c = g;
	}
	Ref r;
	r.cell = c;
	r.id = game.actors[d.actor].id;
	r.idLower = lower(r.id);
	r.type = d.type;
	memcpy(r.pos, pos, sizeof(r.pos));
	memcpy(r.home, pos, sizeof(r.home));
	r.rot[0] = r.rot[1] = 0.0f;
	r.rot[2] = yaw;
	r.actor = d.actor;
	r.obj = game.object(r.id);
	r.moved = true;
	// A box to aim at, a person's size (none before: people placed by scripts couldn't be talked to, nor
	// their bodies searched)
	r.hasBox = true;
	memcpy(r.start, pos, sizeof(r.pos));
	r.start[3] = yaw;
	r.boxStart[0] = pos[0] - 30.0f; r.boxStart[1] = pos[1] - 30.0f; r.boxStart[2] = pos[2];
	r.boxStart[3] = pos[0] + 30.0f; r.boxStart[4] = pos[1] + 30.0f; r.boxStart[5] = pos[2] + 128.0f;
	r.fitBox();
	fillContents(game, r, stats.level, (unsigned)rand());
	initActorState(game, r);
	int slot = -1;
	for (size_t k = 0; k < freeRefs.size(); k++)
		if (freeRefs[k].second >= 1)
		{
			slot = freeRefs[k].first;
			freeRefs[k].first++;
			if (--freeRefs[k].second == 0)
				freeRefs.erase(freeRefs.begin() + k);
			break;
		}
	if (slot < 0)
	{
		slot = refs.size();
		refs.emplace_back();
	}
	refs[slot] = std::move(r);
	refs[slot].spawnedRef = true;
	spawned.push_back(slot);
	startScriptFor(slot);
	attachSpawned(slot);
	rebuildLoadedLists();
	logf("world: placed %s at %.0f %.0f %.0f", id.c_str(), pos[0], pos[1], pos[2]);
	return slot;
}

void World::attachSpawned(int ri)
{
	Ref& r = refs[ri];
	LoadedCell* l = r.cell >= 0 ? cells[placeOf(ri)].live : nullptr;
	auto sp = game.spawns.find(r.idLower);
	const ActorDef* def = r.actor >= 0 ? &game.actors[r.actor] : nullptr;
	std::string lib = sp != game.spawns.end() ? sp->second.lib : def ? def->lib : "";
	int skel = sp != game.spawns.end() ? sp->second.skeleton : def ? def->skel : -1;
	if (!l || lib.empty() || skel < 0)
		return;
	float c = cosf(r.rot[2]), s = sinf(r.rot[2]);
	float place[12] = { c, s, 0.0f, r.pos[0], -s, c, 0.0f, r.pos[1], 0.0f, 0.0f, 1.0f, r.pos[2] };
	r.anim = actorsAddFromLibrary(l->actors, l->cell, textures, dataDir, lib, ri, skel, place);
	CellActor ca = {};
	strncpy(ca.id, r.id.c_str(), sizeof(ca.id) - 1);
	strncpy(ca.name, game.actors[r.actor].name.c_str(), sizeof(ca.name) - 1);
	memcpy(ca.pos, r.pos, sizeof(ca.pos));
	ca.yaw = r.rot[2];
	ca.radius = 28.0f;
	ca.height = 128.0f;
	l->cell.actors.push_back(ca);
	r.cellActor = l->cell.actors.size() - 1;
	syncActor(ri);
}

void World::despawn(int ri)
{
	if (ri < 0 || ri >= (int)refs.size() || !refs[ri].spawnedRef)
		return;
	detachActor(ri);
	for (size_t s = 0; s < scripts.size(); s++)
		if (scripts[s].ref == ri && scripts[s].item.empty())
		{
			scripts.erase(scripts.begin() + s);
			break;
		}
	for (size_t k = 0; k < spawned.size(); k++)
		if (spawned[k] == ri)
		{
			spawned.erase(spawned.begin() + k);
			break;
		}
	refs[ri] = Ref();
	refs[ri].cell = -1;
	freeRefs.push_back({ ri, 1 });
	rebuildScriptIndex();
	rebuildLoadedLists();
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
		if (refill && r.cell == c && r.type == "CONT" && r.obj && r.obj->respawn)
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

// Gone for good: a spawned one is freed, a placed one is marked taken (hidden, and kept so by the cell's state)
void World::deleteRef(int ri)
{
	Ref& r = refs[ri];
	if (r.spawnedRef)
	{
		despawn(ri);
		return;
	}
	r.pickedUp = true;
	hideRanges(*this, r);
}

// Opening dialogue: the purse is full again once fBarterGoldResetDelay hours have passed since the last time
void World::restockGold(int ri)
{
	Ref& r = refs[ri];
	if (r.actor < 0 || gameHour - r.lastBarter < game.gmstf("fbartergoldresetdelay", 24.0f))
		return;
	r.gold = game.actors[r.actor].gold;
	r.lastBarter = gameHour;
}

int World::dropItem(const InventoryItem& it, const float pos[3], float yaw, int inCell)
{
	const Object* o = game.object(it.id);
	if (!o || it.count <= 0)
		return -1;
	int c = inCell >= 0 ? inCell : current;
	if (inCell < 0 && c >= 0 && !cells[c].interior)
	{
		int g = gridCell((int)floorf(pos[0] / 8192.0f), (int)floorf(pos[1] / 8192.0f));
		if (g >= 0 && cells[g].live)
			c = g;
	}
	if (c < 0)
		return -1;
	Ref r;
	r.cell = c;
	r.id = it.id;
	r.idLower = lower(it.id);
	r.type = o->type;
	r.obj = o;
	memcpy(r.pos, pos, sizeof(r.pos));
	memcpy(r.home, pos, sizeof(r.home));
	r.rot[0] = r.rot[1] = 0.0f;
	r.rot[2] = yaw;
	r.count = it.count;
	r.moved = true;
	r.dropped = true;
	r.dropState.condition = it.condition;
	r.dropState.soul = it.soul;
	r.dropState.charge = it.charge;
	// A box to aim at: items are small
	r.hasBox = true;
	r.boxMin[0] = pos[0] - 18.0f; r.boxMin[1] = pos[1] - 18.0f; r.boxMin[2] = pos[2];
	r.boxMax[0] = pos[0] + 18.0f; r.boxMax[1] = pos[1] + 18.0f; r.boxMax[2] = pos[2] + 16.0f;
	memcpy(r.start, pos, sizeof(r.pos));
	memcpy(r.boxStart, r.boxMin, sizeof(r.boxMin));
	memcpy(r.boxStart + 3, r.boxMax, sizeof(r.boxMax));
	int slot = -1;
	for (size_t k = 0; k < freeRefs.size(); k++)
		if (freeRefs[k].second >= 1)
		{
			slot = freeRefs[k].first;
			freeRefs[k].first++;
			if (--freeRefs[k].second == 0)
				freeRefs.erase(freeRefs.begin() + k);
			break;
		}
	if (slot < 0)
	{
		slot = refs.size();
		refs.emplace_back();
	}
	refs[slot] = std::move(r);
	refs[slot].spawnedRef = true;
	spawned.push_back(slot);
	rebuildLoadedLists();
	logf("world: dropped %d x %s at %.0f %.0f %.0f", it.count, it.id.c_str(), pos[0], pos[1], pos[2]);
	return slot;
}

int World::dropItemAt(const InventoryItem& it, const float base[3], float yaw, float dist)
{
	float pos[3] = { base[0] + sinf(yaw) * dist, base[1] + cosf(yaw) * dist, base[2] };
	// Onto the floor there, not through it (from a little above the feet)
	float best = -1e9f;
	for (LoadedCell* l : loaded)
	{
		float z;
		if (collisionFloor(l->cell.collision, pos[0], pos[1], base[2] + 60.0f, base[2] - 600.0f, &z) && z > best)
			best = z;
	}
	if (best > -1e9f)
		pos[2] = best;
	return dropItem(it, pos, yaw);
}

// OpenMW's RegionWeather::chooseNewWeather: a number 1..100 walks the chances in weather order; chances that
// sum to under 100 leave Clear for the rest
int World::rollWeather(const std::string& region)
{
	int roll = rand() % 100 + 1, sum = 0;
	for (int k = 0; k <= WEATHER_BLIZZARD; k++)
		if (roll <= (sum += regionChance(region, k)))
			return k;
	return WEATHER_CLEAR;
}

int World::regionChance(const std::string& region, int weather) const
{
	auto mod = regionChances.find(region);
	if (mod != regionChances.end())
		return weather >= 0 && weather < (int)mod->second.size() ? mod->second[weather] : 0;
	auto def = game.regions.find(region);
	return def != game.regions.end() && weather >= 0 && weather < 10 ? def->second.chances[weather] : 0;
}

int World::regionWeather(const std::string& region)
{
	auto f = forcedWeather.find(region);
	if (f == forcedWeather.end())
		f = forcedWeather.emplace(region, rollWeather(region)).first;
	return f->second;
}

// OpenMW's addWeatherTransition: begins now when nothing is changing, else waits behind the change (a later one
// replaces an earlier wait; the one already coming is ignored)
void World::addWeatherTransition(int weather)
{
	if (weatherNext == weatherNow)
	{
		if (weather != weatherNow)
		{
			weatherNext = weather;
			weatherBlend = 0.0f;
		}
	}
	else if (weather != weatherNext)
		weatherQueued = weather;
}

void World::forceWeather(int weather)
{
	weatherNow = weatherNext = weather;
	weatherQueued = -1;
	weatherBlend = 1.0f;
}

void World::setWeatherHere(int weather)
{
	const LevelCell& lc = cells[current];
	if (!lc.interior && !lc.region.empty())
		weatherRegion = lc.region;
	weatherCell = current;
	if (!weatherRegion.empty())
		forcedWeather[weatherRegion] = weather;
	forceWeather(weather);
}

// ChangeWeather: an unknown region or weather does nothing; the weather is the region's until the next expiry,
// and changes at once (or queues) when the region is the player's
void World::changeWeather(const std::string& region, int weather)
{
	if (!game.regions.count(region) || weather < 0 || weather > WEATHER_BLIZZARD)
		return;
	forcedWeather[region] = weather;
	if (region == weatherRegion)
		addWeatherTransition(weather);
}

// ModRegion: the new chances replace the old; the region's weather is rerolled if they no longer allow it
void World::modRegion(const std::string& region, const std::vector<int>& chances)
{
	if (!game.regions.count(region))
		return;
	regionChances[region] = chances;
	auto f = forcedWeather.find(region);
	if (f == forcedWeather.end() || regionChance(region, f->second) == 0)
		forcedWeather[region] = rollWeather(region);
	if (region == weatherRegion)
		addWeatherTransition(forcedWeather[region]);
}

static float weatherDelta(const GameData& g, int w)
{
	return fmaxf(0.001f, g.weatherTypes[std::min(w, (int)g.weatherTypes.size() - 1)].delta);
}

// One gust step (OpenMW calculateWindSpeed): the wind drifts by up to half of what the weather aims for (a
// quarter in rain), never under half or over double it
static float gust(const WeatherType& t, float speed)
{
	float target = fminf(8.0f * t.wind, 70.0f);
	// (a speed left over from another weather, outside this one's range, would never drift back into it)
	if (speed == 0.0f || speed < 0.5f * target || speed > 2.0f * target)
		speed = target;
	float next = (rand() / (float)RAND_MAX - 0.5f) * (t.precip ? 0.5f : 1.0f) * target + speed;
	return next > 0.5f * target && next < 2.0f * target ? next : speed;
}

void World::updateWeather(float dt)
{
	if (game.weatherTypes.empty() || current < 0)
		return;
	int n = (int)game.weatherTypes.size();
	if (weatherSeen < 0.0f)
		weatherSeen = gameHour;
	// the hours gone by since the last look: this frame's, and any skipped over (rest, wait, travel: a skip
	// finishes the change in progress at once)
	float inc = dt * timescale() / 3600.0f;
	float skipped = gameHour - weatherSeen - inc;
	bool fast = fabsf(skipped) > 0.01f;
	weatherSeen = gameHour;
	weatherTimer -= inc + fmaxf(0.0f, skipped);
	bool changed = false;
	if (weatherTimer <= 0.0f)
	{
		// every region forgets its weather; the player's rolls a new one
		forcedWeather.clear();
		weatherTimer += game.weatherHours;
		changed = true;
	}
	const LevelCell& lc = cells[current];
	if (!lc.interior && !lc.region.empty() && lc.region != weatherRegion)
	{
		// walking over a border: a change to the new region's weather; arriving any other way (a door, travel):
		// that weather at once
		bool walked = !weatherRegion.empty() && weatherCell >= 0 && !cells[weatherCell].interior
			&& abs(cells[weatherCell].gx - lc.gx) <= 1 && abs(cells[weatherCell].gy - lc.gy) <= 1;
		weatherRegion = lc.region;
		if (game.regions.count(weatherRegion) && !walked)
		{
			forceWeather(regionWeather(weatherRegion));
			changed = false;
		}
		else
			changed = true;
	}
	weatherCell = current;
	if (changed && game.regions.count(weatherRegion))
		addWeatherTransition(regionWeather(weatherRegion));
	// the change: at the next weather's delta per real second, the time left over going to the one queued
	if (fast)
	{
		weatherNow = weatherQueued >= 0 ? weatherQueued : weatherNext;
		weatherNext = weatherNow;
		weatherQueued = -1;
		weatherBlend = 1.0f;
	}
	else if (weatherNext != weatherNow)
	{
		weatherBlend += dt * weatherDelta(game, weatherNext);
		if (weatherBlend >= 1.0f)
		{
			float over = (weatherBlend - 1.0f) / weatherDelta(game, weatherNext);
			weatherNow = weatherNext;
			weatherNext = weatherQueued >= 0 ? weatherQueued : weatherNext;
			weatherQueued = -1;
			weatherBlend = weatherNext != weatherNow ? fminf(0.999f, over * weatherDelta(game, weatherNext)) : 1.0f;
		}
	}
	// wind: none indoors; outdoors each weather gusts about what it aims for, the two blending while changing
	if (lc.interior)
		windCur = windNxt = windNow = 0.0f;
	else
	{
		const WeatherType& now = game.weatherTypes[std::min(weatherNow, n - 1)];
		if (weatherNext == weatherNow)
		{
			windCur = windNow = gust(now, windNow);
			windNxt = 0.0f;
		}
		else
		{
			const WeatherType& next = game.weatherTypes[std::min(weatherNext, n - 1)];
			windCur = gust(now, windCur);
			windNxt = gust(next, windNxt);
			windNow = windCur * (1.0f - weatherBlend) + windNxt * weatherBlend;
		}
	}
}

// OpenMW's TimeOfDayInterpolator (weather.cpp): which two of a weather's four values (0 sunrise, 1 day, 2 sunset,
// 3 night) hour h blends for colour channel ch (0 sky, 1 fog, 2 ambient, 3 sun), and how much of the second. Each
// channel has its own lead and lag around the day's boundaries
static void timeOfDay(const WeatherClock& c, int ch, float h, int& a, int& b, float& f)
{
	float preSr = c.win[ch][0], postSr = c.win[ch][1], preSs = c.win[ch][2], postSs = c.win[ch][3];
	float nightEnd = c.sunrise, dayStart = c.sunrise + c.sunriseDur, dayEnd = c.sunset, nightStart = c.sunset + c.sunsetDur;
	a = b = 3;
	f = 0.0f;
	if (h < nightEnd - preSr || h > nightStart + postSs)
		return;
	if (h <= dayStart + postSr)
	{
		float dur = dayStart + postSr - nightEnd + preSr, mid = nightEnd - preSr + dur / 2.0f;
		a = 0;
		b = h <= mid ? 3 : 1;
		f = dur > 0.0f ? fabsf(mid - h) / dur * 2.0f : h <= mid ? 0.0f : 1.0f;
		return;
	}
	if (h < dayEnd - preSs)
	{
		a = b = 1;
		return;
	}
	float dur = nightStart + postSs - dayEnd + preSs, mid = dayEnd - preSs + dur / 2.0f;
	a = 2;
	b = h <= mid ? 1 : 3;
	f = dur > 0.0f ? fabsf(mid - h) / dur * 2.0f : h <= mid ? 0.0f : 1.0f;
}

// One weather's colour channel at hour h
static float colourAt(const GameData& g, int w, int kind, int c, float h)
{
	int a, b;
	float f;
	timeOfDay(g.weatherClock, kind, h, a, b, f);
	const WeatherType& t = g.weatherTypes[std::min(w, (int)g.weatherTypes.size() - 1)];
	return t.colors[kind][a][c] * (1.0f - f) + t.colors[kind][b][c] * f;
}

// One weather's land fog depth at hour h: the day's from sunrise to sunset, only the night's differs (OpenMW passes
// the day depth for sunrise, day and sunset)
static float fogDepthAt(const GameData& g, int w, float h)
{
	int a, b;
	float f;
	timeOfDay(g.weatherClock, 1, h, a, b, f);
	const WeatherType& t = g.weatherTypes[std::min(w, (int)g.weatherTypes.size() - 1)];
	return t.fog[a == 3 ? 1 : 0] * (1.0f - f) + t.fog[b == 3 ? 1 : 0] * f;
}

float World::weatherColour(int kind, int c) const
{
	if (game.weatherTypes.empty())
		return game.weather[kind][1][c];
	float h = fmodf(gameHour, 24.0f);
	return colourAt(game, weatherNow, kind, c, h) * (1.0f - weatherBlend) + colourAt(game, weatherNext, kind, c, h) * weatherBlend;
}

float World::weatherFogDepth() const
{
	if (game.weatherTypes.empty())
		return 0.69f;
	float h = fmodf(gameHour, 24.0f);
	return fogDepthAt(game, weatherNow, h) * (1.0f - weatherBlend) + fogDepthAt(game, weatherNext, h) * weatherBlend;
}

int World::weatherShown() const
{
	if (weatherNext == weatherNow || game.weatherTypes.empty())
		return weatherNow;
	float thr = game.weatherTypes[std::min(weatherNext, (int)game.weatherTypes.size() - 1)].rainThreshold;
	return weatherBlend < (thr > 0.0f ? thr : 0.5f) ? weatherNow : weatherNext;
}

bool World::weatherPrecip() const
{
	int w = weatherShown();
	if (game.weatherTypes.empty() || w == WEATHER_ASH)      // (OpenMW: an ash storm does not count)
		return false;
	return game.weatherTypes[std::min(w, (int)game.weatherTypes.size() - 1)].precip || w >= WEATHER_BLIGHT;
}

bool World::weatherStorm() const
{
	return !game.weatherTypes.empty()
		&& game.weatherTypes[std::min(weatherShown(), (int)game.weatherTypes.size() - 1)].wind > game.gmstf("fstromwindspeed", 0.7f);
}

float World::sunVisibility() const
{
	if (game.weatherTypes.empty())
		return 1.0f;
	int n = (int)game.weatherTypes.size();
	const WeatherType& now = game.weatherTypes[std::min(weatherNow, n - 1)];
	if (weatherNext != weatherNow)
	{
		const WeatherType& next = game.weatherTypes[std::min(weatherNext, n - 1)];
		float left = 1.0f - weatherBlend;
		if (left < next.cloudsMax)
		{
			float t = left / next.cloudsMax;
			return (1.0f - t) * now.glare + t * next.glare;
		}
	}
	return now.glare;
}

float World::glareFade() const
{
	const WeatherClock& c = game.weatherClock;
	float h = fmodf(gameHour, 24.0f), end = c.sunset + c.sunsetDur, peak = c.sunrise + (end - c.sunrise) / 2.0f;
	if (h < c.sunrise || h > end)
		return 0.0f;
	return h < peak ? 1.0f - (peak - h) / (peak - c.sunrise) : 1.0f - (h - peak) / (end - peak);
}

bool World::sunOn() const
{
	const WeatherClock& c = game.weatherClock;
	float h = fmodf(gameHour, 24.0f);
	return !(h >= c.sunset + c.sunsetDur || h <= c.sunrise);
}

bool World::useTorches() const
{
	const WeatherClock& c = game.weatherClock;
	float h = fmodf(gameHour, 24.0f);
	return (h < c.sunrise || h > c.sunset + c.sunsetDur) && !weatherPrecip();
}

// OpenMW's night / day mode: by the current weather's glare, not the blend
int World::nightDay() const
{
	const WeatherClock& c = game.weatherClock;
	float h = fmodf(gameHour, 24.0f);
	bool day = h >= c.sunrise && h <= c.sunset + c.sunsetDur, outdoors = current >= 0 && !cells[current].interior;
	if (outdoors && !day)
		return 2;
	float glare = game.weatherTypes.empty() ? 1.0f : game.weatherTypes[std::min(weatherNow, (int)game.weatherTypes.size() - 1)].glare;
	return !outdoors && day && glare >= 0.5f ? 1 : 0;
}

bool World::underWater(float z) const
{
	for (const LoadedCell* l : loaded)
		if (l->cell.hasWater() && z < l->cell.waterZ)
			return true;
	return false;
}

float World::weatherFogScale() const
{
	if (game.weatherTypes.empty() || current < 0 || cells[current].interior)
		return 1.0f;
	// (the hour's part is OpenMW's fog depth; 0.69 is the vanilla day depth the view distance is tuned for)
	return fmaxf(0.25f, fminf(1.0f, 0.69f / fmaxf(0.1f, weatherFogDepth())));
}

// The weather's colours for the hour (OpenMW's interpolation: each channel its own sunrise / sunset windows)
void World::daylight(float land[3], float sky[3], float fog[3]) const
{
	for (int c = 0; c < 3; c++)
		land[c] = sky[c] = 1.0f;
	if (!game.hasWeather || current < 0 || cells[current].interior)
	{
		for (int c = 0; c < 3; c++)
			fog[c] = cells[current].live ? cells[current].live->cell.fog[c] / 255.0f : 0.0f;
		return;
	}
	// The baked light is the day's: scale by now's ambient + sun against the day's (never pitch black)
	for (int c = 0; c < 3; c++)
	{
		fog[c] = weatherColour(1, c);
		float day = game.weather[2][1][c] + 0.6f * game.weather[3][1][c];
		land[c] = fmaxf(0.45f, fminf(1.2f, (weatherColour(2, c) + 0.6f * weatherColour(3, c)) / fmaxf(0.01f, day)));
		sky[c] = fmaxf(0.05f, fminf(1.5f, weatherColour(0, c) / fmaxf(0.01f, game.weather[0][1][c])));
	}
}

void World::refreshStats()
{
	float h = stats.health, m = stats.magicka, f = stats.fatigue;
	recomputeStats();
	stats.health = fminf(h, stats.healthMax);
	stats.magicka = fminf(m, stats.magickaMax);
	stats.fatigue = fminf(f, stats.fatigueMax);
}

const SpellDef* World::enchantmentOf(const InventoryItem& it) const
{
	const Object* o = game.object(it.id);
	if (!o || o->ench.empty())
		return nullptr;
	auto sp = game.spells.find(o->ench);
	return sp != game.spells.end() ? &sp->second : nullptr;
}

float World::chargeOf(const InventoryItem& it) const
{
	const SpellDef* en = enchantmentOf(it);
	return !en ? 0.0f : it.charge < 0.0f ? (float)en->charge : it.charge;
}

void World::markStolen(const std::string& item, int count, const std::string& owner)
{
	std::string id = lower(item), who = lower(owner);
	stolen[id] += count;
	if (who.empty())
		return;
	auto& owners = stolenFrom[id];
	for (auto& o : owners)
		if (o == who)
			return;
	owners.push_back(who);
}

void World::confiscateStolen()
{
	for (auto& s : stolen)
		removeItem(s.first, s.second);
	stolen.clear();
	stolenFrom.clear();
}

int World::factionReaction(const std::string& a, const std::string& b) const
{
	std::string la = lower(a), lb = lower(b);
	int v = 0;
	for (auto& f : game.factions)
		if (lower(f.first) == la)
		{
			auto r = f.second.reactions.find(lb);
			if (r != f.second.reactions.end())
				v = r->second;
		}
	auto m = factionReactions.find(la + "|" + lb);
	return v + (m != factionReactions.end() ? m->second : 0);
}

int World::disposition(int ri, bool clamp) const
{
	const Ref& r = refs[ri];
	if (r.actor < 0 || game.actors[r.actor].creature)
		return r.disposition;
	const ActorDef& a = game.actors[r.actor];
	float x = (float)r.disposition;
	if (!a.race.empty() && lower(a.race) == lower(stats.race))
		x += game.gmstf("fdispracemod", 5.0f);
	x += game.gmstf("fdisppersonalitymult", 0.5f) * (stats.attributes[ATTR_PERSONALITY] - game.gmstf("fdisppersonalitybase", 50.0f));
	// Faction: their faction's reaction to its own members, else to the player's factions (the worst)
	std::string fac = lower(a.faction);
	float reaction = 0.0f;
	int rank = 0;
	if (!fac.empty())
	{
		auto mine = pcRank.find(fac);
		if (mine != pcRank.end())
		{
			if (!pcExpelled.count(fac))
			{
				reaction = (float)factionReaction(fac, fac);
				rank = mine->second;
			}
		}
		else
		{
			bool first = true;
			for (auto& pf : pcRank)
			{
				if (pcExpelled.count(pf.first))
					continue;
				int re = factionReaction(fac, pf.first);
				if (first || re < reaction)
				{
					reaction = (float)re;
					rank = pf.second;
					first = false;
				}
			}
		}
	}
	x += (game.gmstf("fdispfactionrankmult", 0.5f) * rank + game.gmstf("fdispfactionrankbase", 1.0f))
		* game.gmstf("fdispfactionmod", 3.0f) * reaction;
	x -= game.gmstf("fdispcrimemod", 0.0f) * bounty;
	if (pcHasDisease(3) || pcHasDisease(2))
		x += game.gmstf("fdispdiseasemod", -10.0f);
	if (pcWeaponDrawn)
		x += game.gmstf("fdispweapondrawn", -5.0f);
	x += actorEffect(ri, 44);               // Charm (a spell effect on them: it ends)
	return clamp ? std::max(0, std::min(100, (int)x)) : (int)x;
}

bool World::ownedByOther(int ri) const
{
	if (ri < 0 || ri >= (int)refs.size())
		return false;
	const Ref& r = refs[ri];
	if (!r.ownerGlobal.empty())
	{
		auto g = globals.find(r.ownerGlobal);
		if (g != globals.end() && g->second != 0.0f)
			return false;             // rented
	}
	if (!r.owner.empty())
		return r.owner != "player";
	if (!r.ownerFaction.empty())
		return pcRankIn(r.ownerFaction) < r.ownerRank;
	return false;
}

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

bool World::followsPlayer(int ri) const
{
	if (ri < 0 || ri >= (int)refs.size() || refs[ri].cell < 0 || refs[ri].dead || refs[ri].actor < 0)
		return false;
	const Ref& r = refs[ri];
	if ((r.aiPackage == AIPKG_FOLLOW || r.aiPackage == AIPKG_ESCORT) && r.aiTarget == "player")
		return true;
	for (auto& e : effects)
		if (e.ref == ri)
			return true;
	return false;
}

void World::detachActor(int ri)
{
	Ref& r = refs[ri];
	ActorSet* set = r.anim >= 0 || r.cellActor >= 0 ? actorsOf(ri) : nullptr;
	Cell* cell = r.cellActor >= 0 ? cellOf(ri) : nullptr;
	if (set && r.anim >= 0 && r.anim < (int)set->actors.size())
		set->actors[r.anim].ref = -1;          // no longer drawn
	if (cell && r.cellActor >= 0 && r.cellActor < (int)cell->actors.size())
		cell->actors[r.cellActor].radius = 0.0f;   // nor in the way
	r.anim = r.cellActor = -1;
}

void World::relocate(int ri, int cell, const float pos[3], float yaw)
{
	Ref& r = refs[ri];
	if (cell < 0)
		return;
	if (placeOf(ri) != cell && r.actor >= 0)
		detachActor(ri);
	bool newPlace = placeOf(ri) != cell;
	memcpy(r.pos, pos, sizeof(r.pos));
	memcpy(r.home, pos, sizeof(r.home));
	r.fitBox();
	r.rot[2] = yaw;
	r.moved = true;
	r.path.clear();
	if (r.spawnedRef)
		r.cell = cell;
	r.at = cell == r.cell ? -1 : cell;
	auto listed = std::find(spawned.begin(), spawned.end(), ri);
	if (r.at >= 0 && listed == spawned.end())
		spawned.push_back(ri);
	else if (r.at < 0 && !r.spawnedRef && listed != spawned.end())
		spawned.erase(listed);              // back in its own cell: listed there again (not twice)
	if (newPlace && r.actor >= 0 && cells[cell].live)
		attachSpawned(ri);
	else if (r.actor >= 0)
		syncActor(ri);
	rebuildLoadedLists();
	logf("world: %s moved to %s", r.idLower.c_str(), cells[cell].name.c_str());
}

void World::snapToFloor(float pos[3])
{
	float best = -1e9f;
	for (LoadedCell* l : loaded)
	{
		float z;
		if (collisionFloor(l->cell.collision, pos[0], pos[1], pos[2] + 20.0f, pos[2] - 600.0f, &z) && z > best)
			best = z;
	}
	if (best > -1e9f && best < pos[2])
		pos[2] = best;
}

// OpenMW's PlaceAtMe: every one goes to the same spot, the first free direction (asked, then the one a
// quarter turn back, half a turn, the other side) with nothing in the way; items need no such check
void World::placeAt(const std::string& id, int count, float dist, int dir, const float base[3], float yaw)
{
	if (count <= 0 || dir < 0 || dir > 3)
		return;
	const bool actor = game.spawns.count(lower(id)) != 0;
	const Object* item = actor ? nullptr : game.object(lower(id));
	if (!actor && (!item || (item->type != "MISC" && item->type != "WEAP" && item->type != "ARMO" && item->type != "CLOT"
		&& item->type != "ALCH" && item->type != "INGR" && item->type != "BOOK" && item->type != "APPA"
		&& item->type != "LOCK" && item->type != "PROB" && item->type != "REPA" && item->type != "LIGH")))
	{
		monitorOnce(("placeat:" + id).c_str(), "PlaceAtMe of %s: can't place that kind of record", id.c_str());
		return;
	}
	float pos[3] = { base[0], base[1], base[2] };
	for (int t = 0; t < 4; t++)
	{
		// the asked direction, then +3, +2, +1 (mod 4)
		int d = (dir + (t == 0 ? 0 : 4 - t)) % 4;
		float a = yaw + (d == 1 ? 3.14159265f : d == 2 ? -1.5707963f : d == 3 ? 1.5707963f : 0.0f);
		pos[0] = base[0] + sinf(a) * dist;
		pos[1] = base[1] + cosf(a) * dist;
		pos[2] = base[2];
		if (!actor)
			break;
		float from[3] = { pos[0], pos[1], pos[2] + 30.0f }, to[3] = { base[0], base[1], base[2] + 20.0f };
		if (lineOfSight(from, to))
			break;
	}
	// Onto the floor there
	float best = -1e9f;
	for (LoadedCell* l : loaded)
	{
		float z;
		if (collisionFloor(l->cell.collision, pos[0], pos[1], base[2] + 150.0f, base[2] - 600.0f, &z) && z > best)
			best = z;
	}
	if (best > -1e9f)
		pos[2] = best;
	for (int k = 0; k < count; k++)
	{
		if (actor)
			spawnActor(id, pos, yaw);       // facing the way the one it was placed from does
		else
			dropItem({ lower(id), 1, false }, pos, yaw);
	}
}

int World::refsInMemory() const
{
	int n = 0;
	for (auto& c : cells)
		if (c.refsLoaded)
			n += c.refCount;
	return n;
}

void World::rebuildScriptIndex()
{
	for (auto& r : refs)
		r.script = -1;
	for (size_t k = 0; k < scripts.size(); k++)
		if (scripts[k].ref >= 0 && scripts[k].item.empty())
			refs[scripts[k].ref].script = k;
}

void World::evictCell(int c)
{
	LevelCell& lc = cells[c];
	std::string state = cellStateText(c);
	if (!state.empty())
		cellStates[lc.file] = state;
	// References placed while playing in this cell go (they aren't kept)
	for (size_t k = 0; k < spawned.size();)
	{
		int i = spawned[k];
		if (refs[i].cell != c || !refs[i].spawnedRef)
		{
			k++;
			continue;
		}
		if (refs[i].dropped)
		{
			if (!refs[i].pickedUp)
			{
				DroppedItem d;
				d.item = { refs[i].idLower, refs[i].count, false, refs[i].dropState.condition, refs[i].dropState.soul,
					refs[i].dropState.charge };
				d.cell = lc.file;
				memcpy(d.pos, refs[i].pos, sizeof(d.pos));
				d.yaw = refs[i].rot[2];
				droppedWaiting.push_back(d);
			}
		}
		else if (!refs[i].ally)
		{
			PendingSpawn p = { refs[i].idLower, lc.file, { refs[i].pos[0], refs[i].pos[1], refs[i].pos[2] },
				refs[i].rot[2], refs[i].health, refs[i].dead };
			pendingSpawns.push_back(p);
			logf("world: placed %s waits for %s", p.id.c_str(), p.cell.c_str());
		}
		for (size_t s = 0; s < scripts.size(); s++)
			if (scripts[s].ref == i && scripts[s].item.empty())
			{
				scripts.erase(scripts.begin() + s);
				break;
			}
		refs[i] = Ref();
		refs[i].cell = -1;
		freeRefs.push_back({ i, 1 });
		spawned.erase(spawned.begin() + k);
	}
	int first = lc.refBase, last = lc.refBase + lc.refCount;
	// Their scripts stop (the locals went into the state); carried items' scripts carry on alone
	std::vector<ScriptInstance> kept;
	kept.reserve(scripts.size());
	for (auto& s : scripts)
	{
		if (s.ref >= first && s.ref < last)
		{
			if (s.item.empty())
				continue;
			s.ref = -1;
		}
		kept.push_back(std::move(s));
	}
	scripts = std::move(kept);
	for (int i = first; i < last; i++)
	{
		refs[i] = Ref();
		refs[i].cell = -1;
	}
	if (lc.refCount)
		freeRefs.push_back({ first, lc.refCount });
	for (int i = lc.pathBase; i < lc.pathBase + lc.pathCount; i++)
	{
		// the neighbours' links into these points go too (the slots get reused)
		for (int nb : pathPoints[i].links)
			if (nb < lc.pathBase || nb >= lc.pathBase + lc.pathCount)
			{
				auto& l = pathPoints[nb].links;
				l.erase(std::remove_if(l.begin(), l.end(), [&](int x) { return x >= lc.pathBase && x < lc.pathBase + lc.pathCount; }),
					l.end());
			}
		pathPoints[i].links.clear();
		pathPoints[i].cell = -1;
	}
	if (lc.pathCount)
		freePaths.push_back({ lc.pathBase, lc.pathCount });
	logf("world: %s objects out of memory (%d, %s)", lc.file.c_str(), lc.refCount,
		state.empty() ? "unchanged" : "changes kept");
	lc.refsLoaded = false;
	lc.refBase = lc.refCount = lc.pathBase = lc.pathCount = 0;
	rebuildScriptIndex();
}

int World::evictCells(int budget, const std::function<bool(int)>& inUse)
{
	int total = refsInMemory(), evicted = 0;
	while (total > budget)
	{
		int best = -1;
		for (size_t c = 0; c < cells.size(); c++)
		{
			const LevelCell& lc = cells[c];
			if (!lc.refsLoaded || lc.live || (int)c == current || (best >= 0 && lc.lastUsed >= cells[best].lastUsed))
				continue;
			bool busy = false;
			for (int i = lc.refBase; i < lc.refBase + lc.refCount && !busy; i++)
				busy = inUse(i);
			// ... or one of its references stands in another cell now (PositionCell)
			for (int s : spawned)
				busy |= refs[s].cell == (int)c && refs[s].at >= 0;
			// ... or a path someone in a loaded cell is walking
			for (size_t i = 0; i < refs.size() && !busy; i++)
				if (refs[i].cell >= 0 && active(i))
					for (int pt : refs[i].path)
						busy |= pt >= lc.pathBase && pt < lc.pathBase + lc.pathCount;
			if (!busy)
				best = c;
		}
		if (best < 0)
			break;
		total -= cells[best].refCount;
		evictCell(best);
		evicted++;
	}
	return evicted;
}

bool World::ensureRefs(int cellIndex)
{
	if (cellIndex < 0 || cells[cellIndex].refsLoaded)
		return cellIndex >= 0;
	ParsedRefs p;
	u64 start = osGetTime();
	if (!parseRefs(*this, cellIndex, p))
		return false;
	appendRefs(*this, cellIndex, p);
	logf("world: read %s objects (%d) in %llu ms", cells[cellIndex].file.c_str(), cells[cellIndex].refCount,
		osGetTime() - start);
	return true;
}

// The game data a session leaves behind (World::keepGame) for the next one from the same folder: loading a
// save needn't parse 32 MB of it again (16 s), nor lay a second copy over a heap the first one fragmented
static GameData* s_keptGame = nullptr;
static std::string s_keptDir;

void World::keepGame()
{
	delete s_keptGame;
	// what this session made (potions, enchantments, spells: "mw3ds_..." ids) goes; a save brings its own
	for (auto it = game.objects.begin(); it != game.objects.end();)
		it = it->first.compare(0, 6, "mw3ds_") == 0 ? game.objects.erase(it) : std::next(it);
	for (auto it = game.spells.begin(); it != game.spells.end();)
		it = it->first.compare(0, 6, "mw3ds_") == 0 ? game.spells.erase(it) : std::next(it);
	s_keptGame = new GameData(std::move(game));
	g_gameGeneration++;
	s_keptDir = dataDir ? dataDir : "";
}

bool World::load(const char* dir, const std::string& startCell)
{
	u64 loadStart = osGetTime();
	dataDir = dir;
	char path[256];
	snprintf(path, sizeof(path), "%s/game.json", dir);
	if (s_keptGame && s_keptDir == dir)
	{
		game = std::move(*s_keptGame);
		delete s_keptGame;
		s_keptGame = nullptr;
		logf("world: game data kept from the last session");
	}
	else
	{
		logf("world: reading %s", path);
		if (!gameLoad(game, path))
			return false;
	}
	distantLoad(dir);
	if (g_sharedSkeletons.empty())
	{
		snprintf(path, sizeof(path), "%s/cells/skeletons.skl", dir);
		skeletonsLoad(path);
	}

	// Older data has only the starting cell
	if (game.cells.empty())
		game.cells.push_back({ game.cellName, cellFileName(game.cellName), true });
	for (auto& c : game.cells)
	{
		LevelCell lc;
		lc.name = c.name;
		lc.file = c.file;
		lc.interior = c.interior;
		lc.gx = c.gx;
		lc.gy = c.gy;
		lc.sea = c.sea;
		lc.noSleep = c.noSleep;
		lc.region = c.region;
		if (!c.interior)
			gridIndex[((long long)c.gx << 32) ^ (unsigned)c.gy] = cells.size();
		cells.push_back(lc);
	}
	// Cells' objects are read when a cell is first needed (ensureRefs), not all at start
	for (auto& g : game.globals)
		globals[g.first] = g.second.value;
	recomputeStats();
	logf("world: %d cells, game data in %llu ms", (int)cells.size(), osGetTime() - loadStart);
	int start = startCell.empty() ? -1 : cellIndex(startCell);
	if (!startCell.empty() && start < 0)
		logf("world: start cell %s is not in the level", startCell.c_str());
	if (start < 0)
		start = cellIndex(game.cellName);
	start = start >= 0 ? start : 0;
	// Outdoors the streaming follows the player: start them in the middle of their cell
	if (!cells[start].interior)
	{
		player.feet[0] = (cells[start].gx + 0.5f) * 8192.0f;
		player.feet[1] = (cells[start].gy + 0.5f) * 8192.0f;
	}
	return enterCell(start);
}

Actor* World::actorOf(int ri)
{
	ActorSet* set = actorsOf(ri);
	return set && refs[ri].anim >= 0 ? &set->actors[refs[ri].anim] : nullptr;
}

// Puts an NPC's mesh and collision where its reference is (after moving) and lays the dead down
void World::syncActor(int ri)
{
	Ref& r = refs[ri];
	r.fitBox();
	Cell* c = cellOf(ri);
	if (c && r.cellActor >= 0)
	{
		CellActor& ca = c->actors[r.cellActor];
		memcpy(ca.pos, r.pos, sizeof(ca.pos));
		ca.radius = r.dead ? 0.0f : 28.0f;
	}
	Actor* a = actorOf(ri);
	if (!a)
		return;
	if (r.moved)
		actorSetPlacement(*a, r.pos[0], r.pos[1], r.pos[2], r.rot[2]);
	ActorSet& set = *actorsOf(ri);
	// a corpse that was already lying there is laid down at once; one that has just died keeps playing its fall
	int group = a->group;
	AnimMode mode = a->mode;
	if (r.dead && actorPlay(set, *a, "Death1", ANIM_HOLD) && (a->group != group || a->mode != mode))
		a->time = actorSkeleton(set, a->skeleton).groups[a->group].stop;
}

void World::refreshHidden()
{
	for (LoadedCell* l : loaded)
	{
		const LevelCell& lc = cells[l->index];
		for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
			if (refs[i].visible())
				showRanges(*this, refs[i]);
			else
				hideRanges(*this, refs[i]);
	}
}

int World::cellIndex(const std::string& name) const
{
	std::string l = lower(name);
	for (size_t i = 0; i < cells.size(); i++)
		if (lower(cells[i].name) == l || cells[i].file == l)
			return i;
	return -1;
}

int World::gridCell(int gx, int gy) const
{
	auto it = gridIndex.find(((long long)gx << 32) ^ (unsigned)gy);
	return it == gridIndex.end() ? -1 : it->second;
}

bool World::isStartCell() const
{
	std::string l = lower(game.cellName);
	return lower(cells[current].name) == l || cells[current].file == l;
}

bool World::sameSpace(int a, int b) const
{
	const LevelCell& ca = cells[placeOf(a)];
	const LevelCell& cb = cells[placeOf(b)];
	return placeOf(a) == placeOf(b) || (!ca.interior && !cb.interior);
}

Scene World::scene()
{
	Scene s;
	for (LoadedCell* l : loaded)
		s.cells.push_back(&l->cell);
	s.here = current >= 0 && cells[current].live ? &cells[current].live->cell : nullptr;
	return s;
}

// Makes a freshly loaded cell part of the world: its references find their meshes
static void integrateLevelCell(World& w, int index, LoadedCell* l)
{
	LevelCell& lc = w.cells[index];
	w.respawnCell(index);               // before it is live: revived actors come in standing
	lc.live = l;
	w.loaded.push_back(l);
	// Cell files number references within the cell
	for (auto& a : l->actors.actors)
		if (a.ref >= 0)
			a.ref += lc.refBase;
	for (auto& d : l->cell.doors)
		d.ref += lc.refBase;
	// (one that stands in another cell now keeps its mesh and place there: its entries here go unused, and
	// that cell's, when it is loaded too, are left alone)
	for (size_t k = 0; k < l->actors.actors.size(); k++)
		if (l->actors.actors[k].ref >= 0)
		{
			if (w.refs[l->actors.actors[k].ref].at >= 0)
				l->actors.actors[k].ref = -1;
			else
				w.refs[l->actors.actors[k].ref].anim = k;
		}
	// Cell::actors lists the cell's NPCs in reference order
	int cylinder = 0;
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
		if (w.refs[i].actor >= 0 && cylinder < (int)l->cell.actors.size())
		{
			if (w.refs[i].at >= 0)
				l->cell.actors[cylinder].radius = 0.0f;
			else
				w.refs[i].cellActor = cylinder;
			cylinder++;
		}
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
	{
		if (w.refs[i].at >= 0)
		{
			if (!w.cells[w.refs[i].at].live)
				w.refs[i].anim = w.refs[i].cellActor = -1;
			continue;                      // it stands in another cell now
		}
		if (w.refs[i].relib)
		{
			w.detachActor(i);              // a leveled pick: drawn from its own library, not the cell's
			w.attachSpawned(i);
			continue;
		}
		w.syncActor(i);
		if (!w.refs[i].visible())
			hideRanges(w, w.refs[i]);
	}
	lc.lastUsed = ++w.useTick;
	for (int i : w.spawned)
		if (w.placeOf(i) == index)
			w.attachSpawned(i);
	w.rebuildLoadedLists();
}

// Reads a cell's files (safe on the streaming thread: touches nothing shared but the locked
// linear heap and texture cache)
static bool readLevelCell(const char* dataDir, const std::string& file, TextureCache& cache, LoadedCell* l)
{
	if (!cellLoad(l->cell, dataDir, (file + ".cel").c_str(), cache))
	{
		cellFree(l->cell, cache);
		return false;
	}
	actorsLoad(l->actors, cellPath(dataDir, file, ".act").c_str(), dataDir, l->cell.textureNames);
	if (threadGetCurrent())
		g_workerAt = "readLevelCell: actors read";
	return true;
}

static bool loadLevelCell(World& w, int index)
{
	MARK("loadLevelCell");
	LevelCell& lc = w.cells[index];
	if (lc.live)
		return true;
	if (!w.ensureRefs(index))
		return false;
	LoadedCell* l = new LoadedCell();
	l->index = index;
	logf("world: loading %s", lc.file.c_str());
	if (!readLevelCell(w.dataDir, lc.file, w.textures, l))
	{
		delete l;
		return false;
	}
	integrateLevelCell(w, index, l);
	return true;
}

// ---- Streaming thread: one exterior cell at a time, while the game keeps running

struct StreamJob
{
	int index;
	LoadedCell* cell;
	std::string file;
	const char* dataDir;
	TextureCache* cache;
	const World* world;
	bool parseObjects = false;        // the cell's objects weren't read yet: read them on the thread too
	ParsedRefs objects;
	bool objectsOk = false;
	Thread thread = nullptr;
	volatile bool done = false;
	bool ok = false;
	u64 started = 0;
};

static void streamWorker(void* arg)
{
	StreamJob* j = (StreamJob*)arg;
	crashHandlerInstall(1);
	g_workerAt = "parseRefs";
	if (j->parseObjects)
		j->objectsOk = parseRefs(*j->world, j->index, j->objects);
	g_workerAt = "readLevelCell";
	j->ok = readLevelCell(j->dataDir, j->file, *j->cache, j->cell);
	g_workerAt = "done";
	__sync_synchronize();
	j->done = true;
}

// Takes in a finished (or, with wait, the running) job: integrates the cell if it's still wanted
static void finishStreamJob(World& w, bool wait)
{
	StreamJob* j = w.streamJob;
	if (!j || (!wait && !j->done))
		return;
	MARK("finishStreamJob: join");
	threadJoin(j->thread, U64_MAX);
	MARK("finishStreamJob: integrate");
	threadFree(j->thread);
	w.streamJob = nullptr;
	if (j->parseObjects && j->objectsOk)
		appendRefs(w, j->index, j->objects);            // no-op if they were read meanwhile
	const LevelCell& c = w.cells[j->index];
	const LevelCell* here = w.current >= 0 ? &w.cells[w.current] : nullptr;
	bool wanted = here && !here->interior && abs(c.gx - here->gx) <= 1 && abs(c.gy - here->gy) <= 1 && !c.live
		&& c.refsLoaded;
	if (j->ok && wanted)
	{
		integrateLevelCell(w, j->index, j->cell);
		logf("world: streamed %s in %llu ms (in the background), textures %lu KB, linear free %lu KB",
			j->file.c_str(), osGetTime() - j->started, w.textures.bytes / 1024, linearSpaceFree() / 1024);
	}
	else
	{
		if (j->ok)
		{
			actorsFree(j->cell->actors);
			cellFree(j->cell->cell, w.textures);
		}
		else
			logf("world: failed to load %s", j->file.c_str());
		delete j->cell;
	}
	delete j;
}

static void startStreamJob(World& w, int index)
{
	StreamJob* j = new StreamJob();
	j->index = index;
	j->cell = new LoadedCell();
	j->cell->index = index;
	j->file = w.cells[index].file;
	j->dataDir = w.dataDir;
	j->cache = &w.textures;
	j->world = &w;
	j->parseObjects = !w.cells[index].refsLoaded;
	j->started = osGetTime();
	// Below the main thread's priority: it runs while the main thread waits for the GPU
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	j->thread = threadCreate(streamWorker, j, 64 * 1024, prio + 1, -2, false);
	if (!j->thread)
	{
		// No thread could be made: load it right here instead
		logf("world: no streaming thread, loading %s now", j->file.c_str());
		if (w.ensureRefs(index) && readLevelCell(w.dataDir, j->file, w.textures, j->cell))
			integrateLevelCell(w, index, j->cell);
		else
			delete j->cell;
		delete j;
		return;
	}
	w.streamJob = j;
}

static void unloadLevelCell(World& w, int index)
{
	MARK("unloadLevelCell");
	LevelCell& lc = w.cells[index];
	if (!lc.live)
		return;
	// The index buffers holding hidden references go away with the geometry
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
	{
		w.refs[i].savedIndices.clear();
		w.refs[i].savedCol.clear();          // the collision goes too; hidden again when it loads
		if (w.refs[i].at >= 0)
			continue;                        // standing in another cell: drawn there
		w.refs[i].anim = w.refs[i].cellActor = -1;
		w.refs[i].ai = AI_IDLE;
	}
	for (int i : w.spawned)
		if (w.placeOf(i) == index)
		{
			w.refs[i].anim = w.refs[i].cellActor = -1;
			w.refs[i].ai = AI_IDLE;
		}
	actorsFree(lc.live->actors);
	cellFree(lc.live->cell, w.textures);
	for (size_t i = 0; i < w.loaded.size(); i++)
		if (w.loaded[i] == lc.live)
		{
			w.loaded.erase(w.loaded.begin() + i);
			break;
		}
	delete lc.live;
	lc.live = nullptr;
	w.rebuildLoadedLists();
}

void World::freeAll()
{
	finishStreamJob(*this, true);
	while (!loaded.empty())
		unloadLevelCell(*this, loaded.back()->index);
}

bool World::enterCell(int index)
{
	current = index;
	if (!cells[index].interior)
	{
		streamExterior(true);
		logf("world: outside in %s (%d cells loaded), linear free %lu KB", cells[index].name.c_str(),
			(int)loaded.size(), linearSpaceFree() / 1024);
		return cells[index].live != nullptr;
	}
	finishStreamJob(*this, true);
	for (int i = (int)loaded.size() - 1; i >= 0; i--)
		if (loaded[i]->index != index)
			unloadLevelCell(*this, loaded[i]->index);
	bool ok = loadLevelCell(*this, index);
	logf("world: entered %s (%d refs), textures %lu KB, linear free %lu KB", cells[index].name.c_str(),
		cells[index].refCount, textures.bytes / 1024, linearSpaceFree() / 1024);
	return ok;
}

// Outdoors an actor on its way somewhere (following, escorting, travelling) who walked over into a cell that
// stays loaded goes on in that one when its own is freed. OpenMW's World::moveObject puts an actor in the
// exterior cell under it at every step; here only as the old cell goes, so pacing at a border loads nothing.
// A follower vanished with the cell it started in, two cells behind the player
static void handOverWalkers(World& w, int index, const LevelCell& here)
{
	const LevelCell& lc = w.cells[index];
	std::vector<int> walkers;
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
		if (w.refs[i].at < 0)
			walkers.push_back(i);
	for (int i : w.spawned)
		if (w.placeOf(i) == index)
			walkers.push_back(i);
	for (int i : walkers)
	{
		Ref& r = w.refs[i];
		if (r.cell < 0 || r.actor < 0 || r.dead
			|| (r.aiPackage != AIPKG_FOLLOW && r.aiPackage != AIPKG_ESCORT && r.aiPackage != AIPKG_TRAVEL))
			continue;
		int g = w.gridCell((int)floorf(r.pos[0] / 8192.0f), (int)floorf(r.pos[1] / 8192.0f));
		if (g < 0 || g == index || !w.cells[g].live || abs(w.cells[g].gx - here.gx) > 1 || abs(w.cells[g].gy - here.gy) > 1)
			continue;
		// (where it stands and its home stay as they are)
		float pos[3] = { r.pos[0], r.pos[1], r.pos[2] }, home[3] = { r.home[0], r.home[1], r.home[2] };
		w.relocate(i, g, pos, r.rot[2]);
		memcpy(r.home, home, sizeof(home));
	}
}

void World::streamExterior(bool all)
{
	MARK("streamExterior");
	if (current < 0 || cells[current].interior)
		return;
	// Walked into another grid cell?
	int gx = (int)floorf(player.feet[0] / 8192.0f), gy = (int)floorf(player.feet[1] / 8192.0f);
	int under = gridCell(gx, gy);
	if (under >= 0 && under != current)
	{
		current = under;
		logf("world: outside in %s (%d %d)", cells[current].name.c_str(), gx, gy);
	}
	const LevelCell& c = cells[current];
	// Free what is no longer next to the player (and any interior)
	bool freed = false;
	for (int i = (int)loaded.size() - 1; i >= 0; i--)
	{
		const LevelCell& l = cells[loaded[i]->index];
		if (l.interior || abs(l.gx - c.gx) > 1 || abs(l.gy - c.gy) > 1)
		{
			if (!l.interior)
				handOverWalkers(*this, loaded[i]->index, c);
			unloadLevelCell(*this, loaded[i]->index);
			freed = true;
		}
	}
	// Load the missing neighbours, the player's own cell first: all now (behind a loading screen),
	// or one at a time on the streaming thread
	finishStreamJob(*this, all);
	static const int order[9][2] = { {0, 0}, {0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1} };
	for (auto& o : order)
	{
		int ci = gridCell(c.gx + o[0], c.gy + o[1]);
		if (ci < 0 || cells[ci].live)
			continue;
		if (o[0] == 0 && o[1] == 0)
		{
			// Never walk around in a cell that isn't there: the player's own cell loads now,
			// the neighbours follow on the streaming thread (they start past the fog anyway)
			finishStreamJob(*this, true);
			if (!cells[ci].live && !loadLevelCell(*this, ci))
				logf("world: failed to load %s", cells[ci].file.c_str());
			continue;
		}
		// (not in the update that freed cells: their memory comes back after the next frame begins)
		if (!streamJob && !freed)
			startStreamJob(*this, ci);
		break;
	}
}

void World::startScriptFor(int ri)
{
	Ref& ref = refs[ri];
	if (!ref.obj || ref.obj->script.empty() || ref.script >= 0)
		return;
	const Script* s = game.script(ref.obj->script);
	if (!s)
		return;
	ScriptInstance inst;
	inst.script = s;
	inst.locals.assign(s->localNames.size(), 0.0f);
	inst.ref = ri;
	ref.script = scripts.size();
	scripts.push_back(std::move(inst));
}

int World::startGlobalScript(const std::string& name, int target)
{
	std::string l = lower(name);
	for (size_t i = 0; i < scripts.size(); i++)
		if (scripts[i].ref < 0 && scripts[i].item.empty() && lower(scripts[i].script->name) == l)
		{
			// (one that is running already keeps its reference, as in OpenMW)
			if (!scripts[i].running)
				scripts[i].target = target;
			scripts[i].running = true;
			return i;
		}
	const Script* s = game.script(l);
	if (!s)
	{
		logf("script: StartScript %s: not found", name.c_str());
		monitorOnce(("startscript:" + name).c_str(), "StartScript %s: no such script", name.c_str());
		return -1;
	}
	ScriptInstance inst;
	inst.script = s;
	inst.locals.assign(s->localNames.size(), 0.0f);
	inst.target = target;
	scripts.push_back(std::move(inst));
	return scripts.size() - 1;
}

void World::stopGlobalScript(const std::string& name)
{
	std::string l = lower(name);
	for (auto& s : scripts)
		if (s.ref < 0 && s.item.empty() && lower(s.script->name) == l)
			s.running = false;
}

// Like findRef, but also reads the objects of cells the level's index says hold that id
bool World::deferEnable(const std::string& id, bool on)
{
	if (findRef(id) >= 0)
		return false;
	std::string l = lower(id);
	auto it = game.refCells.find(l);
	if (it == game.refCells.end())
		return false;
	for (int c : it->second)
		if (c >= 0 && c < (int)cells.size() && !cells[c].refsLoaded)
		{
			lazyEnabled[l] = on;
			return true;
		}
	return false;
}

int World::findRefAnywhere(const std::string& id)
{
	int r = findRef(id);
	if (r >= 0)
		return r;
	auto it = game.refCells.find(lower(id));
	if (it == game.refCells.end())
		return -1;
	bool read = false;
	for (int c : it->second)
		if (c >= 0 && c < (int)cells.size() && !cells[c].refsLoaded)
			read |= ensureRefs(c);
	return read ? findRef(id) : -1;
}

int World::findRef(const std::string& id) const
{
	std::string l = lower(id);
	int other = -1;
	for (size_t i = 0; i < refs.size(); i++)
		if (refs[i].idLower == l)
		{
			if (active(i))
				return i;
			if (other < 0)
				other = i;
		}
	return other;
}

int World::findActorRef(int actor) const
{
	for (size_t i = 0; i < refs.size(); i++)
		if (refs[i].actor == actor)
			return i;
	return -1;
}

// Hiding a reference turns its triangles into degenerate ones in the shared index buffers
static void hideRanges(World& w, Ref& ref)
{
	// Cells that aren't loaded hide their references when they load
	LoadedCell* live = w.cells[ref.cell].live;
	if (live && ref.colCount)
		collisionHideTris(live->cell.collision, ref.colFirst, ref.colCount, ref.savedCol);
	if (!ref.savedIndices.empty() || !live)
		return;
	for (auto& r : ref.ranges)
	{
		if (r.batch < 0 || r.batch >= (int)live->cell.batches.size())
			continue;
		CellBatch& b = live->cell.batches[r.batch];
		if (!b.indices || r.first + r.count > b.numIndices)
			continue;
		u16* idx = b.indices + r.first;
		ref.savedIndices.insert(ref.savedIndices.end(), idx, idx + r.count);
		for (u32 i = 0; i < r.count; i++)
			idx[i] = idx[0];
		GSPGPU_FlushDataCache(idx, r.count * 2);
	}
	if (ref.ranges.empty())
		ref.savedIndices.push_back(0);   // marker: hidden
}

static void showRanges(World& w, Ref& ref)
{
	LoadedCell* live = w.cells[ref.cell].live;
	if (!live)
		return;
	if (ref.colCount)
		collisionShowTris(live->cell.collision, ref.colFirst, ref.colCount, ref.savedCol);
	size_t pos = 0;
	for (auto& r : ref.ranges)
	{
		if (pos + r.count > ref.savedIndices.size())
			break;
		if (r.batch < 0 || r.batch >= (int)live->cell.batches.size())
			continue;
		const CellBatch& b = live->cell.batches[r.batch];
		if (!b.indices || r.first + r.count > b.numIndices)
			continue;
		u16* idx = b.indices + r.first;
		memcpy(idx, &ref.savedIndices[pos], r.count * 2);
		GSPGPU_FlushDataCache(idx, r.count * 2);
		pos += r.count;
	}
	ref.savedIndices.clear();
}

void World::setEnabled(int ri, bool on)
{
	if (ri < 0)
		return;
	Ref& ref = refs[ri];
	ref.enabled = on;
	if (ref.visible())
		showRanges(*this, ref);
	else
		hideRanges(*this, ref);
}

void World::pickUp(int ri)
{
	Ref& ref = refs[ri];
	if (ref.dropped || !ref.dropState.soul.empty() || ref.dropState.charge >= 0.0f)
	{
		// Dropped items, and placed ones with a soul or a charge, come back whole
		InventoryItem it = { ref.idLower, ref.count > 0 ? ref.count : 1, false, ref.dropState.condition,
			ref.dropState.soul, ref.dropState.charge };
		if (ref.dropped)
			despawn(ri);
		else
		{
			ref.pickedUp = true;
			hideRanges(*this, ref);
		}
		if (it.condition < 0 && it.soul.empty() && it.charge < 0.0f)
			addItem(it.id, it.count);
		else
			addStack(it);
		return;
	}
	ref.pickedUp = true;
	hideRanges(*this, ref);
	// The object's script keeps running while it is carried (mark it before addItem,
	// which would otherwise start a second copy for the item)
	if (ref.script >= 0)
	{
		ScriptInstance& s = scripts[ref.script];
		s.item = ref.idLower;
		if (float* v = s.local("onpcadd"))
			*v = 1.0f;
	}
	addItem(ref.id, ref.count > 0 ? ref.count : 1);
}

int World::itemCount(const std::string& id) const
{
	std::string l = lower(id);
	int n = 0;
	for (auto& it : inventory)
		if (it.id == l)
			n += it.count;           // filled soul gems count too
	return n;
}

void World::addItem(const std::string& id, int count)
{
	std::string l = lower(id);
	// Gold of any size is gold_001 in an inventory (a gold_100 added is one gold)
	if (l == "gold_005" || l == "gold_010" || l == "gold_025" || l == "gold_100")
		l = "gold_001";
	// A new one stacks with plain ones only: not onto a worn stack, nor a worn-down or part-charged one
	for (auto& it : inventory)
		if (it.id == l && it.soul.empty() && !it.equipped && it.condition < 0 && it.charge < 0.0f)
		{
			it.count += count;
			return;
		}
	inventory.push_back({ l, count, false });
	// Items handed over by scripts or dialogue start their own scripts
	const Object* o = game.object(l);
	if (o && !o->script.empty())
	{
		for (auto& s : scripts)
			if (s.item == l)
			{
				s.running = true;         // (it stopped when the last one left)
				return;
			}
		const Script* sc = game.script(o->script);
		if (sc)
		{
			ScriptInstance inst;
			inst.script = sc;
			inst.locals.assign(sc->localNames.size(), 0.0f);
			inst.item = l;
			if (float* v = inst.local("onpcadd"))
				*v = 1.0f;
			scripts.push_back(std::move(inst));
		}
	}
}

int World::removeItem(const std::string& id, int count)
{
	std::string l = lower(id);
	int want = count;
	bool wornGone = false;
	// Empty soul gems go before filled ones
	for (int pass = 0; pass < 2 && count > 0; pass++)
		for (size_t i = 0; i < inventory.size() && count > 0;)
			if (inventory[i].id == l && inventory[i].soul.empty() == (pass == 0))
			{
				int n = std::min(count, inventory[i].count);
				inventory[i].count -= n;
				count -= n;
				if (inventory[i].count <= 0)
				{
					wornGone |= inventory[i].equipped;
					inventory.erase(inventory.begin() + i);
				}
				else
					i++;
			}
			else
				i++;
	if (wornGone)
		refreshStats();                // its constant effects go with it
	// An item's script runs while it is carried: with the last one gone it stops
	if (want > count && itemCount(l) == 0)
		for (auto& s : scripts)
			if (s.item == l)
				s.running = false;
	return want - count;
}

int World::refItemCount(const Ref& r, const std::string& id) const
{
	std::string l = lower(id);
	int n = 0;
	for (auto& c : r.contents)
		if (lower(c.second) == l)
			n += stockCount(c.first);
	return n;
}

void World::refAddItem(Ref& r, const std::string& id, int count)
{
	std::string l = lower(id);
	// (a new one joins only a stack that is whole: not a worn one, a filled gem or a part-charged one)
	for (auto& c : r.contents)
		if (lower(c.second) == l && c.plain())
		{
			c.first += c.first < 0 ? -count : count;       // (a restocking quantity stays one)
			return;
		}
	r.contents.emplace_back(count, l);
}

// A whole item into a container (OpenMW's ContainerStore::stacks): onto a stack of the player's (not a restocking
// one) with the same id and soul when both are whole (no wear, full charge), else a stack of its own
void World::refAddStack(Ref& r, const InventoryItem& item)
{
	bool whole = item.condition < 0 && item.charge < 0.0f;
	if (whole)
		for (auto& c : r.contents)
			if (c.first > 0 && lower(c.second) == item.id && c.soul == item.soul && c.condition < 0 && c.charge < 0.0f)
			{
				c.first += item.count;
				return;
			}
	ContentItem c(item.count, item.id);
	c.condition = item.condition;
	c.soul = item.soul;
	c.charge = item.charge;
	r.contents.push_back(std::move(c));
}

// Count of a container's stack into the inventory: a plain one as addItem (gold, the item's script), one with wear,
// a soul or a charge whole (addStack)
void World::takeStack(const ContentItem& c, int count)
{
	if (c.plain())
		addItem(c.second, count);
	else
		addStack({ lower(c.second), count, false, c.condition, c.soul, c.charge });
}

int World::refRemoveItem(Ref& r, const std::string& id, int count)
{
	std::string l = lower(id);
	int removed = 0;
	// (empty soul gems go before filled ones, as World::removeItem)
	for (int pass = 0; pass < 2 && removed < count; pass++)
		for (size_t i = 0; i < r.contents.size() && removed < count;)
			if (lower(r.contents[i].second) == l && r.contents[i].soul.empty() == (pass == 0))
			{
				int n = std::min(count - removed, stockCount(r.contents[i].first));
				r.contents[i].first += r.contents[i].first < 0 ? n : -n;
				removed += n;
				if (r.contents[i].first == 0)
					r.contents.erase(r.contents.begin() + i);
				else
					i++;
			}
			else
				i++;
	return removed;
}

// A whole item joins the inventory: onto a stack of the same id and soul that is whole and not worn, else a new one
void World::addStack(const InventoryItem& item)
{
	for (auto& it : inventory)
		if (it.id == item.id && it.soul == item.soul && !it.equipped && it.condition < 0 && it.charge < 0.0f
			&& item.condition < 0 && item.charge < 0.0f)
		{
			it.count += item.count;
			return;
		}
	inventory.push_back(item);
}

// A creature's soul goes into the smallest empty soul gem that holds it (capacity: the gem's value
// x fSoulgemMult)
bool World::trapSoul(const std::string& creature, int soul)
{
	int best = -1;
	float bestCap = 1e30f;
	for (size_t i = 0; i < inventory.size(); i++)
	{
		const InventoryItem& it = inventory[i];
		if (!it.soul.empty() || it.id.compare(0, 13, "misc_soulgem_") != 0)
			continue;
		const Object* o = game.object(it.id);
		float cap = (o ? o->value : 0) * game.gmstf("fsoulgemmult", 3.0f);
		if (cap >= soul && cap < bestCap)
		{
			best = i;
			bestCap = cap;
		}
	}
	if (best < 0)
		return false;
	std::string gem = inventory[best].id;
	removeItem(gem, 1);
	InventoryItem filled = { gem, 1, false };
	filled.soul = lower(creature);
	addStack(filled);
	return true;
}

// The soul size a gem holds (0 = empty or not a gem)
int World::soulValue(const InventoryItem& it) const
{
	if (it.soul.empty())
		return 0;
	for (auto& a : game.actors)
		if (lower(a.id) == it.soul)
			return a.soul;
	return 0;
}

void World::setItemScriptLocal(const std::string& itemId, const char* name, float value)
{
	for (auto& s : scripts)
		if (s.item == itemId)
			if (float* v = s.local(name))
				*v = value;
}

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

const RaceDef* World::race() const
{
	for (auto& r : game.races)
		if (lower(r.id) == lower(stats.race))
			return &r;
	return nullptr;
}

const ClassDef* World::playerClass() const
{
	for (auto& c : game.classes)
		if (lower(c.id) == lower(stats.cls))
			return &c;
	return nullptr;
}

const BirthDef* World::birthsign() const
{
	for (auto& b : game.birthsigns)
		if (lower(b.id) == lower(stats.birthsign))
			return &b;
	return nullptr;
}

// Character creation rules: race base attributes, +10 for the class's favored ones;
// skills 5, minors +10, majors +25, race bonuses, +5 for the class specialization;
// constant abilities from race and birthsign on top.
void World::recomputeStats()
{
	const RaceDef* r = race();
	const ClassDef* c = playerClass();
	const BirthDef* b = birthsign();
	std::fill(std::begin(stats.attrBelow), std::end(stats.attrBelow), 0);
	std::fill(std::begin(stats.skillBelow), std::end(stats.skillBelow), 0);
	for (int i = 0; i < 8; i++)
	{
		stats.attributes[i] = r ? r->attributes[i][stats.female ? 1 : 0] : 40;
		if (c && (c->attributes[0] == i || c->attributes[1] == i))
			stats.attributes[i] += 10;
		stats.attrCreation[i] = stats.attributes[i];
	}
	for (int s = 0; s < 27; s++)
	{
		stats.skills[s] = 5;
		if (c)
		{
			for (int k = 0; k < 5; k++)
			{
				if (c->major[k] == s) stats.skills[s] += 25;
				if (c->minor[k] == s) stats.skills[s] += 10;
			}
			if (s < (int)game.skills.size() && game.skills[s].specialization == c->specialization)
				stats.skills[s] += 5;
		}
	}
	if (r)
		for (auto& bonus : r->skillBonus)
			if (bonus.first >= 0 && bonus.first < 27)
				stats.skills[bonus.first] += bonus.second;
	for (int s = 0; s < 27; s++)
		stats.skillCreation[s] = stats.skills[s];

	float magickaMult = game.gmstf("fpcbasemagickamult", 1.0f);
	std::vector<std::string> abilities;
	if (r) abilities.insert(abilities.end(), r->spells.begin(), r->spells.end());
	// Worn constant-effect enchantments count as abilities while worn
	for (auto& it : inventory)
		if (it.equipped)
			if (const SpellDef* en = enchantmentOf(it))
				if (en->type == ENCH_CONSTANT)
					abilities.push_back(en->id);
	if (b) abilities.insert(abilities.end(), b->spells.begin(), b->spells.end());
	// The player's own abilities and diseases (AddSpell, caught from creatures) work the same way
	for (auto& id : stats.spells)
	{
		auto sp = game.spells.find(lower(id));
		if (sp != game.spells.end() && sp->second.type >= 1 && sp->second.type <= 3)
			abilities.push_back(id);
	}
	this->abilities.clear();
	for (auto& id : abilities)
	{
		auto it = game.spells.find(lower(id));
		if (it == game.spells.end() || ((it->second.type < 1 || it->second.type > 3) && it->second.type != ENCH_CONSTANT))
			continue;                                  // abilities, diseases, worn constant enchantments
		// Corprus: its drains and fortifies grow a point a day since it was caught
		int scale = 1;
		for (auto& e : it->second.effects)
			if (e.effect == 132)
				scale = corprusLevel();
		for (const SpellEffect& e0 : it->second.effects)
		{
			SpellEffect e = e0;
			if (e.effect == 132 && corprusSince < -1.5f)
				continue;                                  // cured: the Corprus effect is gone
			if (e.effect == 17 || e.effect == 79)
				e.min *= scale;
			if (e.effect == 79 && e.attribute >= 0 && e.attribute < 8) stats.attributes[e.attribute] += e.min;
			else if (e.effect == 83 && e.skill >= 0 && e.skill < 27) stats.skills[e.skill] += e.min;
			else if (e.effect == 84) magickaMult += e.min / 10.0f;
			else if (e.effect == 17 && e.attribute >= 0 && e.attribute < 8) stats.attributes[e.attribute] -= e.min;
			else if (e.effect == 21 && e.skill >= 0 && e.skill < 27) stats.skills[e.skill] -= e.min;
			else          // resistances, weaknesses, night eye ...: read while they last (for good)
				this->abilities.push_back({ e.effect, e.attribute, e.skill, (float)e.min, 1e9f, it->second.name });
		}
	}
	for (int i = 0; i < 8; i++)
	{
		stats.attributes[i] = stats.attributes[i] + stats.attrBonus[i] > 100 ? 100 : stats.attributes[i] + stats.attrBonus[i];
		// OpenMW cuts (base - damage) to a whole number, so part of a point of damage takes a whole point
		stats.attributes[i] = std::max(0, stats.attributes[i] - (int)ceilf(stats.attrDamage[i] - 0.001f));
	}
	for (int k = 0; k < 27; k++)
	{
		stats.skills[k] = stats.skills[k] + stats.skillBonus[k] > 100 ? 100 : stats.skills[k] + stats.skillBonus[k];
		stats.skills[k] = std::max(0, stats.skills[k] - (int)ceilf(stats.skillDamage[k] - 0.001f));
	}
	const int* a = stats.attributes;
	// OpenMW: base health is fixed at creation (floor of half Strength + Endurance) and grows only by the level-up
	// gains; raised, fortified or drained attributes do not move it (spec/derived.md)
	stats.healthMax = floorf(0.5f * (stats.attrCreation[ATTR_STRENGTH] + stats.attrCreation[ATTR_ENDURANCE])) + stats.healthBonus;
	stats.magickaMax = a[ATTR_INTELLIGENCE] * magickaMult;
	stats.fatigueMax = a[ATTR_STRENGTH] + a[ATTR_WILLPOWER] + a[ATTR_AGILITY] + a[ATTR_ENDURANCE];
	stats.health = stats.healthMax;
	stats.magicka = stats.magickaMax;
	stats.fatigue = stats.fatigueMax;
	reapplyEffects();
}

float World::distanceToPlayer(int ri) const
{
	const Ref& r = refs[ri];
	if (!active(ri) || (cells[placeOf(ri)].interior && placeOf(ri) != current))
		return 100000.0f;
	float dx = r.pos[0] - player.feet[0], dy = r.pos[1] - player.feet[1], dz = r.pos[2] - player.feet[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static const int kMonthDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

// The calendar is the Day / Month / Year / DaysPassed globals (scripts set them; time moving on advances them)
GameDate World::date() const
{
	auto value = [&](const char* id, int fallback) {
		auto it = globals.find(id);
		if (it != globals.end())
			return (int)it->second;
		auto g = game.globals.find(id);
		return g != game.globals.end() ? (int)g->second.value : fallback;
	};
	GameDate d;
	d.day = value("day", 16);
	d.month = value("month", 7);
	d.year = value("year", 427);
	d.daysPassed = value("dayspassed", 1);
	d.hour = gameHour - floorf(gameHour / 24.0f) * 24.0f;
	if (d.month < 0 || d.month > 11)
		d.month = 0;
	return d;
}

float World::timescale() const
{
	auto it = globals.find("timescale");
	return it != globals.end() ? it->second : 30.0f;
}

// OpenMW's DateTimeManager setters (datetimemanager.cpp): a day past the month's end wraps into the next months,
// a month past 11 carries years and cuts the day to its length, an hour past 24 moves the day but not DaysPassed
void World::setGlobal(const std::string& name, float value)
{
	if (name == "gamehour")
	{
		float h = fmaxf(0.0f, value);
		int days = (int)(h / 24.0f);
		float hour = fminf(fmodf(h, 24.0f), 23.99999809f);
		gameHour = floorf(gameHour / 24.0f) * 24.0f + hour;
		weatherSeen = gameHour;          // (setting the hour is not time passing: no weather change)
		globals[name] = hourWritten = hour;
		if (days > 0)
			setGlobal("day", globals["day"] + (float)days);
		return;
	}
	if (name == "day")
	{
		int day = std::max(1, (int)value), month = std::max(0, std::min(11, (int)globals["month"]));
		int year = (int)globals["year"];
		for (;;)
		{
			if (day <= kMonthDays[month])
				break;
			day -= kMonthDays[month];
			if (month < 11)
				month++;
			else
			{
				month = 0;
				year++;
			}
		}
		globals["day"] = (float)day;
		globals["month"] = (float)month;
		globals["year"] = (float)year;
		return;
	}
	if (name == "month")
	{
		int m = std::max(0, (int)value);
		int month = m % 12;
		globals["year"] = globals["year"] + (float)(m / 12);
		globals["month"] = (float)month;
		if (globals["day"] > kMonthDays[month])
			globals["day"] = (float)kMonthDays[month];
		return;
	}
	if (name == "year" || name == "dayspassed")
	{
		globals[name] = truncf(value);
		return;
	}
	globals[name] = value;
}

// Whole days gone by: the day moves on through the months, years carry, days passed counts them
static void advanceCalendar(std::unordered_map<std::string, float>& g, int days)
{
	for (int k = 0; k < days; k++)
	{
		int month = std::max(0, std::min(11, (int)g["month"]));
		if (++g["day"] > kMonthDays[month])
		{
			g["day"] = 1.0f;
			if (month < 11)
				g["month"] = (float)(month + 1);
			else
			{
				g["month"] = 0.0f;
				g["year"] += 1.0f;
			}
		}
		g["dayspassed"] += 1.0f;
	}
}

void World::syncTime()
{
	float& gh = globals["gamehour"];
	// (a script that set GameHour another way)
	if (hourWritten >= 0.0f && fabsf(gh - hourWritten) > 0.001f)
		setGlobal("gamehour", gh);
	int today = (int)floorf(gameHour / 24.0f);
	if (dayCounted >= 0 && today > dayCounted)
		advanceCalendar(globals, today - dayCounted);
	dayCounted = today;
	globals["gamehour"] = hourWritten = gameHour - floorf(gameHour / 24.0f) * 24.0f;
}

void World::update(float dt)
{
	time += dt;
	gameHour += dt / 3600.0f * timescale();
	syncTime();
	updateWeather(dt);
	// Pose only actors near enough to be drawn (the towns have many more than the office)
	// and pose the far ones less often (their re-skinning is rarer too, renderer.cpp)
	float reach = here().fogEnd > 0.0f ? fminf(here().fogEnd * drawScale, kActorViewDistance) + 200.0f : 4200.0f;
	static u32 frame = 0;
	frame++;
	for (LoadedCell* l : loaded)
	{
		for (size_t k = 0; k < l->actors.actors.size(); k++)
		{
			Actor& a = l->actors.actors[k];
			float dx = a.place[3] - player.feet[0], dy = a.place[7] - player.feet[1];
			float d2 = dx * dx + dy * dy;
			if (a.ref < 0 || !refs[a.ref].visible() || d2 >= reach * reach)
				continue;
			a.pendingDt += dt;
			if ((frame + k) % actorPoseEvery(d2) == 0)
			{
				actorAnimate(l->actors, a, a.pendingDt);
				a.pendingDt = 0.0f;
			}
		}
		l->cell.blockers.clear();
	}
	for (int i : loadedDoors)
	{
		Ref& ref = refs[i];
		Cell& cell = cells[ref.cell].live->cell;
		if (ref.type != "DOOR")
		{
			// Moved / turned by scripts (gates, Dwemer doors, platforms): the box goes where it went, and
			// blocks while it's turned less than ~17 degrees from where it started
			float turned = fmaxf(fabsf(angleDelta(ref.rot[2], ref.start[3])),
				fmaxf(fabsf(angleDelta(ref.rot[0], ref.startRot[0])), fabsf(angleDelta(ref.rot[1], ref.startRot[1]))));
			ref.fitBox();
			if (ref.visible() && ref.hasBox && turned < 0.3f)
			{
				CollisionBox box;
				memcpy(box.min, ref.boxMin, sizeof(box.min));
				memcpy(box.max, ref.boxMax, sizeof(box.max));
				cell.blockers.push_back(box);
			}
			continue;
		}
		{
			float step = 1.6f * dt;    // ~1 s to swing open
			if (ref.doorAngle < ref.doorTarget) ref.doorAngle = fminf(ref.doorTarget, ref.doorAngle + step);
			if (ref.doorAngle > ref.doorTarget) ref.doorAngle = fmaxf(ref.doorTarget, ref.doorAngle - step);
			if (ref.visible() && ref.hasBox && fabsf(ref.doorAngle) < 0.3f)
			{
				CollisionBox box;
				memcpy(box.min, ref.boxMin, sizeof(box.min));
				memcpy(box.max, ref.boxMax, sizeof(box.max));
				cell.blockers.push_back(box);
			}
		}
	}
	for (int i : loadedActors)
	{
		if (refs[i].helloTimer > 0.0f)
			refs[i].helloTimer -= dt;
		refs[i].fitBox();          // however it moved (walking, scripts, a cell's state, a save)
	}
}

static bool isActivatable(const Ref& r);

float angleDelta(float a, float b)
{
	float d = fmodf(a - b, 6.2831853f);
	if (d > 3.14159265f) d -= 6.2831853f;
	if (d < -3.14159265f) d += 6.2831853f;
	return d;
}

void World::rebuildLoadedLists()
{
	loadedActors.clear();
	loadedDoors.clear();
	loadedPickables.clear();
	forLoadedRefs([&](int i) {
		const Ref& r = refs[i];
		if (r.actor >= 0)
			loadedActors.push_back(i);
		if (r.doorMesh >= 0)
			loadedDoors.push_back(i);
		if (r.hasBox && r.obj)
			loadedPickables.push_back(i);        // whether it's usable right now is checked when picking
	});
}

static bool isActivatable(const Ref& r)
{
	if (!r.visible() || !r.hasBox || !r.obj)
		return false;
	const std::string& t = r.type;
	if (t == "ACTI" && r.obj->name.empty())
		return false;                      // nameless activators are script triggers ("chargen stuff room")
	if (t == "NPC_" || t == "DOOR" || t == "CONT" || t == "ACTI")
		return true;
	if (t == "CREA")
		return r.dead;                     // search the body
	if (t == "LIGH")
		return (r.obj->flags & 2) != 0;    // only carryable lights
	return isItemType(t);
}

bool actorAimable(const World& w, int ri)
{
	const Ref& r = w.refs[ri];
	if (!isActivatable(r))
		return false;
	float mid = fminf(64.0f, (r.boxMax[2] - r.boxMin[2]) * 0.5f);
	float at[3] = { r.pos[0], r.pos[1], r.pos[2] + mid };
	float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
	float d = sqrtf(dx * dx + dy * dy);
	float dir[3] = { d > 1.0f ? dx / d : 0.0f, d > 1.0f ? dy / d : 1.0f, 0.0f };
	// its own box only (what stands in front of it is another matter): the slab test, from 100 units off
	float tmin = 0.0f, tmax = 200.0f;
	for (int k = 0; k < 3; k++)
	{
		float e = at[k] - dir[k] * 100.0f;
		if (fabsf(dir[k]) < 1e-6f)
		{
			if (e < r.boxMin[k] || e > r.boxMax[k])
				return false;
			continue;
		}
		float t1 = (r.boxMin[k] - e) / dir[k], t2 = (r.boxMax[k] - e) / dir[k];
		tmin = fmaxf(tmin, fminf(t1, t2));
		tmax = fminf(tmax, fmaxf(t1, t2));
	}
	return tmin <= tmax;
}

// The ray's entry distance into a reference's world bounds (slightly padded for thin items), or -1 for a miss
static float rayBoxEntry(const Ref& r, const float eye[3], const float dir[3], float maxT)
{
	float tmin = 0.0f, tmax = maxT;
	for (int k = 0; k < 3; k++)
	{
		float lo = r.boxMin[k] - 2.0f, hi = r.boxMax[k] + 2.0f;
		if (fabsf(dir[k]) < 1e-6f)
		{
			if (eye[k] < lo || eye[k] > hi)
				return -1.0f;
			continue;
		}
		float t1 = (lo - eye[k]) / dir[k], t2 = (hi - eye[k]) / dir[k];
		if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
		tmin = fmaxf(tmin, t1);
		tmax = fminf(tmax, t2);
		if (tmin > tmax)
			return -1.0f;
	}
	return tmin;
}

// Whether the eye is outside a reference's box but within the reach of it
static bool boxInReach(const Ref& r, const float eye[3], float reach)
{
	float d2 = 0.0f;
	for (int k = 0; k < 3; k++)
	{
		float c = fmaxf(r.boxMin[k], fminf(eye[k], r.boxMax[k])) - eye[k];
		d2 += c * c;
	}
	// An eye inside the box is inside the thing (the prison ship's model is one activator 1800 units wide, and its
	// top deck is inside it): nothing to point at from in there
	return d2 <= reach * reach && d2 != 0.0f;
}

int worldPick(const World& w, const float eye[3], const float dir[3], float reach)
{
	int best = -1;
	float bestT = reach;
	for (int i : w.loadedPickables)
	{
		const Ref& r = w.refs[i];
		if (!isActivatable(r) || !boxInReach(r, eye, reach))
			continue;
		float t = rayBoxEntry(r, eye, dir, bestT);
		if (t >= 0.0f && t < bestT)
		{
			bestT = t;
			best = i;
		}
	}
	// A body's box is a crude one (the Journal of Tarhiel lands where he does): an item whose box lies within the
	// body's and is on the ray is what the crosshair means, as the body's mesh would let the ray through to it
	if (best >= 0 && w.refs[best].actor >= 0 && w.refs[best].dead)
	{
		const Ref& body = w.refs[best];
		int item = -1;
		float itemT = reach;
		for (int i : w.loadedPickables)
		{
			const Ref& r = w.refs[i];
			if (r.actor >= 0 || !isItemType(r.type) || !isActivatable(r) || !boxInReach(r, eye, reach))
				continue;
			bool inside = true;
			for (int k = 0; k < 3 && inside; k++)
				inside = r.boxMax[k] >= body.boxMin[k] && r.boxMin[k] <= body.boxMax[k];
			float t = inside ? rayBoxEntry(r, eye, dir, itemT) : -1.0f;
			if (t >= 0.0f && t < itemT)
			{
				itemT = t;
				item = i;
			}
		}
		if (item >= 0)
			best = item;
	}
	// A container's box reaches well past it (a basket's, a barrel's, turned: OpenMW picks by the meshes' collision
	// shapes instead): an item the ray reaches is what the crosshair means unless the ray crosses the container's middle
	// on the way to it. The middle: a column 0.7 of the box's narrower half-width round its centre (a round thing turned
	// fills about 0.76 of it). Ajira's report lies between two baskets in the Balmora guild
	if (best >= 0 && w.refs[best].type == "CONT")
	{
		const Ref& box = w.refs[best];
		int item = -1;
		float itemT = reach;
		for (int i : w.loadedPickables)
		{
			const Ref& r = w.refs[i];
			if (r.actor >= 0 || !isItemType(r.type) || !isActivatable(r) || !boxInReach(r, eye, reach))
				continue;
			float t = rayBoxEntry(r, eye, dir, itemT);
			if (t >= 0.0f && t < itemT)
			{
				itemT = t;
				item = i;
			}
		}
		if (item >= 0)
		{
			float cx = (box.boxMin[0] + box.boxMax[0]) * 0.5f, cy = (box.boxMin[1] + box.boxMax[1]) * 0.5f;
			float rc = 0.7f * 0.5f * fminf(box.boxMax[0] - box.boxMin[0], box.boxMax[1] - box.boxMin[1]);
			// the ray's nearest point to the column's axis before it reaches the item
			float d2 = dir[0] * dir[0] + dir[1] * dir[1];
			float t = d2 > 1e-6f ? ((cx - eye[0]) * dir[0] + (cy - eye[1]) * dir[1]) / d2 : 0.0f;
			t = fmaxf(0.0f, fminf(itemT, t));
			float px = eye[0] + dir[0] * t - cx, py = eye[1] + dir[1] * t - cy, pz = eye[2] + dir[2] * t;
			bool crosses = px * px + py * py <= rc * rc && pz >= box.boxMin[2] && pz <= box.boxMax[2];
			if (!crosses)
				best = item;
		}
	}
	return best;
}
